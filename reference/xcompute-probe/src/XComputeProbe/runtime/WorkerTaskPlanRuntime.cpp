#include "pch.h"

#include "WorkerTaskPlanRuntime.h"

#include "../ProbeResult.h"
#include "WorkerGraphRuntime.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr wchar_t const* TaskPlanSchemaVersion = L"worker-task-plan-0.29";
        constexpr wchar_t const* TaskPlanRunControlSchemaVersion = L"worker-task-plan-run-control-0.1";

        std::wstring Utf8ToWide(std::string const& value)
        {
            return winrt::to_hstring(value).c_str();
        }

        std::string WideToUtf8(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::string ReadTextFile(fs::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw WorkerTaskPlanError("file.read_failed", "could not open file for read");
            }
            return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        }

        void WriteTextFile(fs::path const& path, std::string const& content)
        {
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw WorkerTaskPlanError("file.write_failed", "could not open file for write");
            }
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.flush();
            if (!output.good())
            {
                throw WorkerTaskPlanError("file.write_failed", "file write did not complete");
            }
        }

        bool IsSafeProgramId(std::wstring const& value)
        {
            if (value.empty() || value.size() > 64)
            {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'a' && ch <= L'z') ||
                    (ch >= L'A' && ch <= L'Z') ||
                    (ch >= L'0' && ch <= L'9') ||
                    ch == L'_' ||
                    ch == L'-';
            });
        }

        fs::path TaskPlanDirectory(fs::path const& root)
        {
            return root / L"jobs" / L"task-plans";
        }

        fs::path ResolveTaskPlanPath(fs::path const& root, std::wstring const& taskId)
        {
            if (!IsSafeProgramId(taskId))
            {
                throw WorkerTaskPlanError("task_id.invalid", "task_id must be 1..64 chars using letters, digits, underscore, or dash");
            }
            return TaskPlanDirectory(root) / (taskId + L".json");
        }

        bool IsTaskPlanStepStatus(std::wstring const& status)
        {
            return status == L"pending" || status == L"running" || status == L"completed" ||
                status == L"failed" || status == L"skipped";
        }

        void InsertJsonString(JsonObject& object, wchar_t const* name, std::wstring const& value)
        {
            object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
        }

        void InsertJsonNumber(JsonObject& object, wchar_t const* name, uint64_t value)
        {
            object.Insert(name, JsonValue::CreateNumberValue(static_cast<double>(value)));
        }

        void InsertJsonBool(JsonObject& object, wchar_t const* name, bool value)
        {
            object.Insert(name, JsonValue::CreateBooleanValue(value));
        }

        std::wstring BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring GetOptionalString(
            JsonObject const& object,
            wchar_t const* name,
            std::wstring const& fallback = L"")
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedString(name, fallback);
            return std::wstring(value.data(), value.size());
        }

        uint64_t GetOptionalUInt64(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedNumber(name);
            if (value < 0)
            {
                throw WorkerTaskPlanError("argument.invalid_number", "numeric argument cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }

        uint64_t GetBoundedOptionalUInt64(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t fallback,
            uint64_t minimum,
            uint64_t maximum)
        {
            auto value = GetOptionalUInt64(object, name, fallback);
            if (value < minimum || value > maximum)
            {
                throw WorkerTaskPlanError("argument.out_of_range", "numeric argument is outside the supported range");
            }
            return value;
        }

        void AppendOptionalJsonString(
            std::wstring& output,
            JsonObject const& object,
            wchar_t const* name)
        {
            auto value = GetOptionalString(object, name);
            if (!value.empty())
            {
                output += L"," + JsonString(name) + L":" + JsonString(value);
            }
        }

        void AppendOptionalJsonNumber(
            std::wstring& output,
            JsonObject const& object,
            wchar_t const* name)
        {
            if (object.HasKey(name))
            {
                output += L"," + JsonString(name) + L":" +
                    std::to_wstring(GetOptionalUInt64(object, name));
            }
        }

        void AppendOptionalJsonBool(
            std::wstring& output,
            JsonObject const& object,
            wchar_t const* name)
        {
            if (object.HasKey(name))
            {
                output += L"," + JsonString(name) + L":" +
                    BoolJson(object.GetNamedBoolean(name, false));
            }
        }

        std::wstring GenerateSessionId()
        {
            auto ticks = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            uint64_t first = ticks;
            uint64_t second = ticks ^ 0xa5a5a5a55a5a5a5aull;
            try
            {
                std::random_device random;
                first ^= (static_cast<uint64_t>(random()) << 32) ^ random();
                second ^= (static_cast<uint64_t>(random()) << 32) ^ random();
            }
            catch (...)
            {
            }

            std::wostringstream out;
            out << std::hex << std::setfill(L'0')
                << std::setw(16) << first
                << std::setw(16) << second;
            return out.str();
        }

        std::wstring GenerateTaskId()
        {
            auto suffix = GenerateSessionId();
            if (suffix.size() > 16)
            {
                suffix.resize(16);
            }
            return L"task-" + suffix;
        }

        struct TaskPlanCounts
        {
            uint64_t completed = 0;
            uint64_t failed = 0;
            uint64_t running = 0;
            uint64_t pending = 0;
            uint64_t skipped = 0;
            std::wstring status = L"pending";
        };

        TaskPlanCounts CountTaskPlanSteps(JsonArray const& steps)
        {
            TaskPlanCounts counts;
            for (uint32_t i = 0; i < steps.Size(); ++i)
            {
                auto status = GetOptionalString(steps.GetObjectAt(i), L"status", L"pending");
                if (!IsTaskPlanStepStatus(status))
                {
                    throw WorkerTaskPlanError("task_plan.step_status_invalid", "step status is invalid");
                }

                if (status == L"completed")
                {
                    ++counts.completed;
                }
                else if (status == L"skipped")
                {
                    ++counts.completed;
                    ++counts.skipped;
                }
                else if (status == L"failed")
                {
                    ++counts.failed;
                }
                else if (status == L"running")
                {
                    ++counts.running;
                }
                else
                {
                    ++counts.pending;
                }
            }

            counts.status = counts.failed > 0
                ? L"failed"
                : (counts.running > 0
                    ? L"running"
                    : (counts.completed == steps.Size() ? L"completed" : L"pending"));
            return counts;
        }

        TaskPlanCounts RefreshTaskPlanMetadata(
            JsonObject& plan,
            JsonArray const& steps,
            std::wstring const& protocolVersion)
        {
            auto counts = CountTaskPlanSteps(steps);
            InsertJsonString(plan, L"schema_version", TaskPlanSchemaVersion);
            InsertJsonString(plan, L"protocol_version", protocolVersion);
            InsertJsonString(plan, L"status", counts.status);
            InsertJsonNumber(plan, L"step_count", steps.Size());
            InsertJsonNumber(plan, L"completed_count", counts.completed);
            InsertJsonNumber(plan, L"failed_count", counts.failed);
            InsertJsonNumber(plan, L"running_count", counts.running);
            return counts;
        }

        void PersistTaskPlan(fs::path const& planPath, JsonObject& plan)
        {
            WriteTextFile(planPath, WideToUtf8(std::wstring(plan.Stringify().c_str())));
        }

        std::wstring TaskPlanSummaryJson(JsonObject const& plan)
        {
            auto taskId = GetOptionalString(plan, L"task_id");
            auto kind = GetOptionalString(plan, L"kind", L"generic");
            auto status = GetOptionalString(plan, L"status", L"pending");
            auto stepCount = GetOptionalUInt64(plan, L"step_count");
            auto completedCount = GetOptionalUInt64(plan, L"completed_count");
            auto failedCount = GetOptionalUInt64(plan, L"failed_count");
            return L"{\"task_id\":" + JsonString(taskId) +
                L",\"kind\":" + JsonString(kind) +
                L",\"status\":" + JsonString(status) +
                L",\"step_count\":" + std::to_wstring(stepCount) +
                L",\"completed_count\":" + std::to_wstring(completedCount) +
                L",\"failed_count\":" + std::to_wstring(failedCount) +
                L"}";
        }

        std::wstring TaskPlanAutoRunJson(JsonObject const& plan)
        {
            if (!plan.HasKey(L"auto_run"))
            {
                return L"null";
            }
            return std::wstring(plan.GetNamedObject(L"auto_run").Stringify().c_str());
        }

        std::wstring FindTaskPlanOutputSha256(JsonObject const& plan)
        {
            if (!plan.HasKey(L"steps"))
            {
                return L"";
            }

            std::wstring outputSha256;
            auto steps = plan.GetNamedArray(L"steps");
            for (uint32_t i = 0; i < steps.Size(); ++i)
            {
                auto candidate = GetOptionalString(steps.GetObjectAt(i), L"output_sha256");
                if (!candidate.empty())
                {
                    outputSha256 = candidate;
                }
            }
            return outputSha256;
        }

        std::wstring OkBase(
            std::wstring const& protocolVersion,
            std::wstring const& command)
        {
            return L"{\"ok\":true,\"protocol_version\":" + JsonString(protocolVersion) +
                L",\"command\":" + JsonString(command);
        }
    }

    WorkerTaskPlanRuntime::WorkerTaskPlanRuntime(
        std::wstring protocolVersion,
        WorkerTaskPlanActionExecutor actionExecutor)
        : protocolVersion_(std::move(protocolVersion)),
          actionExecutor_(std::move(actionExecutor))
    {
    }

    wchar_t const* WorkerTaskPlanRuntime::SchemaVersion()
    {
        return TaskPlanSchemaVersion;
    }

    wchar_t const* WorkerTaskPlanRuntime::RunControlSchemaVersion()
    {
        return TaskPlanRunControlSchemaVersion;
    }

    bool WorkerTaskPlanRuntime::HandlesCommand(std::wstring const& command)
    {
        return command == L"create_task_plan" ||
            command == L"get_task_plan" ||
            command == L"resume_task_plan" ||
            command == L"advance_task_plan" ||
            command == L"submit_task_plan_run" ||
            command == L"cancel_task_plan_run" ||
            command == L"get_task_plan_status" ||
            command == L"get_task_plan_result" ||
            command == L"update_task_plan_step" ||
            command == L"delete_task_plan" ||
            command == L"list_task_plans";
    }

    std::wstring WorkerTaskPlanRuntime::ExecuteCommand(
        std::wstring const& command,
        JsonObject const& request,
        fs::path const& root)
    {
        if (command == L"create_task_plan") return CreateTaskPlan(request, root);
        if (command == L"get_task_plan") return GetTaskPlan(request, root);
        if (command == L"resume_task_plan") return ResumeTaskPlan(request, root);
        if (command == L"advance_task_plan") return AdvanceTaskPlan(request, root);
        if (command == L"submit_task_plan_run") return SubmitTaskPlanRun(request, root);
        if (command == L"cancel_task_plan_run") return CancelTaskPlanRun(request, root);
        if (command == L"get_task_plan_status") return GetTaskPlanStatus(request, root);
        if (command == L"get_task_plan_result") return GetTaskPlanResult(request, root);
        if (command == L"update_task_plan_step") return UpdateTaskPlanStep(request, root);
        if (command == L"delete_task_plan") return DeleteTaskPlan(request, root);
        if (command == L"list_task_plans") return ListTaskPlans(root);
        throw WorkerTaskPlanError("command.unsupported", "task-plan command is not supported");
    }

    std::wstring WorkerTaskPlanRuntime::ExecuteAction(
        std::wstring const& action,
        JsonObject const& step,
        fs::path const& root) const
    {
        if (!actionExecutor_)
        {
            throw WorkerTaskPlanError("task_plan.executor_unavailable", "task-plan action executor is unavailable");
        }
        return actionExecutor_(action, step, root);
    }
    std::wstring WorkerTaskPlanRuntime::CreateTaskPlan(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            taskId = GenerateTaskId();
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        auto replace = request.GetNamedBoolean(L"replace", false);
        if (fs::is_regular_file(planPath) && !replace)
        {
            throw WorkerTaskPlanError("task_plan.exists", "task plan already exists");
        }

        auto kind = GetOptionalString(request, L"kind", L"generic");
        if (!IsSafeProgramId(kind))
        {
            throw WorkerTaskPlanError("task_plan.kind_invalid", "kind must be 1..64 chars using letters, digits, underscore, or dash");
        }
        auto description = GetOptionalString(request, L"description");
        if (description.size() > 256)
        {
            throw WorkerTaskPlanError("task_plan.description_too_large", "description cannot exceed 256 chars");
        }
        if (!request.HasKey(L"steps"))
        {
            throw WorkerTaskPlanError("task_plan.steps_required", "steps array is required");
        }
        auto steps = request.GetNamedArray(L"steps");
        auto stepCount = steps.Size();
        if (stepCount == 0 || stepCount > WorkerMaxTaskPlanSteps())
        {
            throw WorkerTaskPlanError("task_plan.steps_out_of_range", "task plan must contain 1..64 steps");
        }

        std::vector<std::wstring> stepJson;
        stepJson.reserve(stepCount);
        uint64_t completedCount = 0;
        uint64_t failedCount = 0;
        uint64_t runningCount = 0;
        for (uint32_t i = 0; i < stepCount; ++i)
        {
            auto step = steps.GetObjectAt(i);
            auto stepId = GetOptionalString(step, L"step_id");
            if (!IsSafeProgramId(stepId))
            {
                throw WorkerTaskPlanError("task_plan.step_id_invalid", "step_id must be 1..64 chars using letters, digits, underscore, or dash");
            }
            auto status = GetOptionalString(step, L"status", L"pending");
            if (!IsTaskPlanStepStatus(status))
            {
                throw WorkerTaskPlanError("task_plan.step_status_invalid", "step status is invalid");
            }
            auto label = GetOptionalString(step, L"label", stepId);
            if (label.size() > 160)
            {
                throw WorkerTaskPlanError("task_plan.step_label_too_large", "step label cannot exceed 160 chars");
            }

            if (status == L"completed" || status == L"skipped")
            {
                ++completedCount;
            }
            else if (status == L"failed")
            {
                ++failedCount;
            }
            else if (status == L"running")
            {
                ++runningCount;
            }

            std::wstring item = L"{\"step_id\":" + JsonString(stepId) +
                L",\"status\":" + JsonString(status) +
                L",\"label\":" + JsonString(label) +
                L",\"attempts\":" + std::to_wstring(GetOptionalUInt64(step, L"attempts", 0)) +
                L",\"failures\":" + std::to_wstring(GetOptionalUInt64(step, L"failures", 0));
            AppendOptionalJsonString(item, step, L"action");
            AppendOptionalJsonString(item, step, L"path");
            AppendOptionalJsonString(item, step, L"program_id");
            AppendOptionalJsonString(item, step, L"input_path");
            AppendOptionalJsonString(item, step, L"output_path");
            AppendOptionalJsonString(item, step, L"job_id");
            AppendOptionalJsonString(item, step, L"async_job_id");
            AppendOptionalJsonString(item, step, L"snapshot_path");
            AppendOptionalJsonString(item, step, L"detail");
            AppendOptionalJsonString(item, step, L"data_base64");
            AppendOptionalJsonString(item, step, L"bytecode_base64");
            AppendOptionalJsonString(item, step, L"expected_base64");
            AppendOptionalJsonString(item, step, L"expected_sha256");
            AppendOptionalJsonString(item, step, L"expected_output_sha256");
            AppendOptionalJsonString(item, step, L"output_sha256");
            AppendOptionalJsonString(item, step, L"result_status");
            AppendOptionalJsonString(item, step, L"elements_list");
            AppendOptionalJsonNumber(item, step, L"chunk_offset");
            AppendOptionalJsonNumber(item, step, L"chunk_bytes");
            AppendOptionalJsonNumber(item, step, L"bytes");
            AppendOptionalJsonNumber(item, step, L"expected_bytes");
            AppendOptionalJsonNumber(item, step, L"expected_bytecode_hash32");
            AppendOptionalJsonNumber(item, step, L"elements");
            AppendOptionalJsonNumber(item, step, L"requested_elements");
            AppendOptionalJsonNumber(item, step, L"dispatch_groups");
            AppendOptionalJsonNumber(item, step, L"hash32");
            AppendOptionalJsonNumber(item, step, L"mismatch_count");
            AppendOptionalJsonNumber(item, step, L"shader_bytes");
            AppendOptionalJsonNumber(item, step, L"repeats");
            AppendOptionalJsonNumber(item, step, L"warmup_repeats");
            AppendOptionalJsonNumber(item, step, L"result_count");
            AppendOptionalJsonNumber(item, step, L"aggregate_hash32");
            AppendOptionalJsonNumber(item, step, L"max_mismatch_count");
            AppendOptionalJsonNumber(item, step, L"seed");
            AppendOptionalJsonNumber(item, step, L"iterations");
            AppendOptionalJsonNumber(item, step, L"memory_bytes");
            AppendOptionalJsonNumber(item, step, L"output_bytes");
            AppendOptionalJsonBool(item, step, L"resumable");
            AppendOptionalJsonBool(item, step, L"verified");
            AppendOptionalJsonBool(item, step, L"result_available");
            AppendOptionalJsonBool(item, step, L"expected_persisted");
            AppendOptionalJsonBool(item, step, L"preserve_snapshot");
            AppendOptionalJsonBool(item, step, L"truncate");
            AppendOptionalJsonBool(item, step, L"recursive");
            item += L"}";
            stepJson.push_back(item);
        }

        auto planStatus = failedCount > 0
            ? L"failed"
            : (runningCount > 0 ? L"running" : (completedCount == stepCount ? L"completed" : L"pending"));

        std::wostringstream stepsOut;
        stepsOut << L"[";
        for (size_t i = 0; i < stepJson.size(); ++i)
        {
            if (i != 0)
            {
                stepsOut << L",";
            }
            stepsOut << stepJson[i];
        }
        stepsOut << L"]";

        auto planJson = L"{\"schema_version\":" + JsonString(TaskPlanSchemaVersion) +
            L",\"protocol_version\":" + JsonString(protocolVersion_) +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"kind\":" + JsonString(kind) +
            L",\"description\":" + JsonString(description) +
            L",\"status\":" + JsonString(planStatus) +
            L",\"step_count\":" + std::to_wstring(stepCount) +
            L",\"completed_count\":" + std::to_wstring(completedCount) +
            L",\"failed_count\":" + std::to_wstring(failedCount) +
            L",\"running_count\":" + std::to_wstring(runningCount) +
            L",\"steps\":" + stepsOut.str() +
            L"}";
        WriteTextFile(planPath, WideToUtf8(planJson));

        return OkBase(protocolVersion_, L"create_task_plan") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"path\":" + JsonString(planPath.lexically_relative(root).wstring()) +
            L",\"replaced\":" + BoolJson(replace) +
            L",\"task_plan\":" + planJson +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::GetTaskPlan(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }
        auto planText = Utf8ToWide(ReadTextFile(planPath));
        auto plan = JsonObject::Parse(planText);
        return OkBase(protocolVersion_, L"get_task_plan") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"path\":" + JsonString(planPath.lexically_relative(root).wstring()) +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::ResumeTaskPlan(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }

        auto resetRunning = request.GetNamedBoolean(L"reset_running", false);
        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        auto steps = plan.GetNamedArray(L"steps");
        if (steps.Size() == 0)
        {
            throw WorkerTaskPlanError("task_plan.steps_required", "task plan must contain at least one step");
        }

        uint64_t completedCount = 0;
        uint64_t failedCount = 0;
        uint64_t runningCount = 0;
        uint64_t pendingCount = 0;
        uint64_t skippedCount = 0;
        uint64_t runningResetCount = 0;
        int32_t nextStepIndex = -1;
        bool blocked = false;
        bool mutated = false;
        std::wstring resumeReason = L"completed";
        std::wstring nextStepId;
        std::wstring nextStepStatus;
        std::wstring nextAction;
        std::wstring nextStepJson = L"null";

        for (uint32_t i = 0; i < steps.Size(); ++i)
        {
            auto step = steps.GetObjectAt(i);
            auto status = GetOptionalString(step, L"status", L"pending");
            if (!IsTaskPlanStepStatus(status))
            {
                throw WorkerTaskPlanError("task_plan.step_status_invalid", "step status is invalid");
            }

            if (status == L"running" && resetRunning)
            {
                status = L"pending";
                InsertJsonString(step, L"status", status);
                InsertJsonString(step, L"detail", L"reset from running by resume_task_plan");
                InsertJsonBool(step, L"resumed", true);
                steps.SetAt(i, step);
                ++runningResetCount;
                mutated = true;
            }

            if (status == L"completed")
            {
                ++completedCount;
            }
            else if (status == L"skipped")
            {
                ++completedCount;
                ++skippedCount;
            }
            else if (status == L"failed")
            {
                ++failedCount;
            }
            else if (status == L"running")
            {
                ++runningCount;
            }
            else
            {
                ++pendingCount;
            }

            if (nextStepIndex >= 0 || blocked)
            {
                continue;
            }

            if (status == L"pending" || status == L"running")
            {
                nextStepIndex = static_cast<int32_t>(i);
                resumeReason = status == L"running" ? L"running_step" : L"pending_step";
            }
            else if (status == L"failed")
            {
                nextStepIndex = static_cast<int32_t>(i);
                if (step.GetNamedBoolean(L"resumable", false))
                {
                    resumeReason = L"failed_resumable_step";
                }
                else
                {
                    blocked = true;
                    resumeReason = L"failed_terminal_step";
                }
            }

            if (nextStepIndex >= 0)
            {
                nextStepId = GetOptionalString(step, L"step_id");
                nextStepStatus = status;
                nextAction = GetOptionalString(step, L"action");
                nextStepJson = std::wstring(step.Stringify().c_str());
            }
        }

        auto complete = completedCount == steps.Size() && failedCount == 0 && runningCount == 0 && pendingCount == 0;
        auto canResume = nextStepIndex >= 0 && !blocked && !complete;
        auto terminal = complete || blocked;
        if (complete)
        {
            resumeReason = L"completed";
        }

        auto planStatus = failedCount > 0
            ? L"failed"
            : (runningCount > 0 ? L"running" : (completedCount == steps.Size() ? L"completed" : L"pending"));
        if (mutated)
        {
            InsertJsonString(plan, L"schema_version", TaskPlanSchemaVersion);
            InsertJsonString(plan, L"protocol_version", protocolVersion_);
            InsertJsonString(plan, L"status", planStatus);
            InsertJsonNumber(plan, L"step_count", steps.Size());
            InsertJsonNumber(plan, L"completed_count", completedCount);
            InsertJsonNumber(plan, L"failed_count", failedCount);
            InsertJsonNumber(plan, L"running_count", runningCount);
            WriteTextFile(planPath, WideToUtf8(std::wstring(plan.Stringify().c_str())));
        }

        return OkBase(protocolVersion_, L"resume_task_plan") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"path\":" + JsonString(planPath.lexically_relative(root).wstring()) +
            L",\"task_status\":" + JsonString(planStatus) +
            L",\"can_resume\":" + BoolJson(canResume) +
            L",\"terminal\":" + BoolJson(terminal) +
            L",\"blocked\":" + BoolJson(blocked) +
            L",\"complete\":" + BoolJson(complete) +
            L",\"reset_running\":" + BoolJson(resetRunning) +
            L",\"running_reset_count\":" + std::to_wstring(runningResetCount) +
            L",\"resume_reason\":" + JsonString(resumeReason) +
            L",\"next_step_index\":" + std::to_wstring(nextStepIndex) +
            L",\"next_step_id\":" + JsonString(nextStepId) +
            L",\"next_step_status\":" + JsonString(nextStepStatus) +
            L",\"next_action\":" + JsonString(nextAction) +
            L",\"step_count\":" + std::to_wstring(steps.Size()) +
            L",\"pending_count\":" + std::to_wstring(pendingCount) +
            L",\"running_count\":" + std::to_wstring(runningCount) +
            L",\"completed_count\":" + std::to_wstring(completedCount) +
            L",\"failed_count\":" + std::to_wstring(failedCount) +
            L",\"skipped_count\":" + std::to_wstring(skippedCount) +
            L",\"next_step\":" + nextStepJson +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::AdvanceTaskPlan(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto maxSteps = GetBoundedOptionalUInt64(request, L"max_steps", 1, 1, 16);
        auto resetRunning = request.GetNamedBoolean(L"reset_running", false);
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }

        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        auto steps = plan.GetNamedArray(L"steps");
        if (steps.Size() == 0)
        {
            throw WorkerTaskPlanError("task_plan.steps_required", "task plan must contain at least one step");
        }

        uint64_t runningResetCount = 0;
        if (resetRunning)
        {
            for (uint32_t i = 0; i < steps.Size(); ++i)
            {
                auto step = steps.GetObjectAt(i);
                if (GetOptionalString(step, L"status", L"pending") == L"running")
                {
                    InsertJsonString(step, L"status", L"pending");
                    InsertJsonString(step, L"detail", L"reset from running by advance_task_plan");
                    InsertJsonBool(step, L"resumed", true);
                    steps.SetAt(i, step);
                    ++runningResetCount;
                }
            }
            if (runningResetCount != 0)
            {
                RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
                PersistTaskPlan(planPath, plan);
            }
        }

        uint64_t advancedCount = 0;
        bool blocked = false;
        std::wstring blockedReason;
        std::vector<std::wstring> stepResults;

        while (advancedCount < maxSteps)
        {
            auto counts = RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
            if (counts.completed == steps.Size() && counts.failed == 0 && counts.running == 0 && counts.pending == 0)
            {
                break;
            }

            int32_t stepIndex = -1;
            JsonObject step{ nullptr };
            for (uint32_t i = 0; i < steps.Size(); ++i)
            {
                auto candidate = steps.GetObjectAt(i);
                auto candidateStatus = GetOptionalString(candidate, L"status", L"pending");
                if (candidateStatus == L"completed" || candidateStatus == L"skipped")
                {
                    continue;
                }
                if (candidateStatus == L"running")
                {
                    blocked = true;
                    blockedReason = L"running_step";
                    stepIndex = static_cast<int32_t>(i);
                    step = candidate;
                    break;
                }
                if (candidateStatus == L"failed" && !candidate.GetNamedBoolean(L"resumable", false))
                {
                    blocked = true;
                    blockedReason = L"failed_terminal_step";
                    stepIndex = static_cast<int32_t>(i);
                    step = candidate;
                    break;
                }
                if (candidateStatus == L"pending" || candidateStatus == L"failed")
                {
                    stepIndex = static_cast<int32_t>(i);
                    step = candidate;
                    break;
                }
            }

            if (stepIndex < 0)
            {
                break;
            }
            if (blocked)
            {
                break;
            }

            auto stepId = GetOptionalString(step, L"step_id");
            auto action = GetOptionalString(step, L"action");
            if (stepId.empty())
            {
                throw WorkerTaskPlanError("task_plan.step_id_invalid", "step_id is required");
            }
            if (action.empty())
            {
                blocked = true;
                blockedReason = L"missing_action";
                break;
            }
            if (action != L"write_chunk" &&
                action != L"read_chunk" &&
                action != L"hash_file" &&
                action != L"delete" &&
                action != L"mkdir" &&
                action != L"store_memory_program" &&
                action != L"submit_stored_memory_file_job" &&
                action != L"get_job_status" &&
                action != L"get_job_result" &&
                action != L"purge_job" &&
                action != L"run_d3d11_compute_job" &&
                action != L"run_d3d11_compute_sweep_job" &&
                action != L"run_d3d11_compute_timing_job" &&
                action != L"run_d3d11_fp32_timing_job")
            {
                blocked = true;
                blockedReason = L"unsupported_action";
                break;
            }

            InsertJsonString(step, L"status", L"running");
            InsertJsonNumber(step, L"attempts", GetOptionalUInt64(step, L"attempts", 0) + 1);
            steps.SetAt(static_cast<uint32_t>(stepIndex), step);
            RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
            PersistTaskPlan(planPath, plan);

            try
            {
                std::wstring resultJson;
                bool completedStep = true;
                if (action == L"run_d3d11_fp32_timing_job")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    auto resultCount = GetOptionalUInt64(result, L"result_count");
                    auto totalElementsTimed = GetOptionalUInt64(result, L"total_elements_timed");
                    auto totalBytesTimed = GetOptionalUInt64(result, L"total_bytes_timed");
                    auto totalFp32OpsTimed = GetOptionalUInt64(result, L"total_fp32_ops_timed");
                    auto aggregateHash32 = GetOptionalUInt64(result, L"aggregate_hash32");
                    auto maxMismatchCount = GetOptionalUInt64(result, L"max_mismatch_count");
                    auto gpuTimingSampleCount = GetOptionalUInt64(result, L"gpu_timing_sample_count");
                    auto gpuDisjointCount = GetOptionalUInt64(result, L"gpu_disjoint_count");
                    auto featureLevel = GetOptionalString(result, L"feature_level");
                    auto gpuTimingAvailable = result.GetNamedBoolean(L"gpu_timing_available", false);
                    if (!verified || maxMismatchCount != 0)
                    {
                        throw WorkerTaskPlanError("task_plan.verify_failed", "D3D11 FP32 timing task step verification failed");
                    }
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"detail", L"D3D11 FP32 timing verified");
                    InsertJsonString(step, L"result_status", L"succeeded");
                    InsertJsonString(step, L"feature_level", featureLevel);
                    InsertJsonNumber(step, L"result_count", resultCount);
                    InsertJsonNumber(step, L"elements", totalElementsTimed);
                    InsertJsonNumber(step, L"bytes", totalBytesTimed);
                    InsertJsonNumber(step, L"fp32_ops", totalFp32OpsTimed);
                    InsertJsonNumber(step, L"aggregate_hash32", aggregateHash32);
                    InsertJsonNumber(step, L"max_mismatch_count", maxMismatchCount);
                    InsertJsonNumber(step, L"gpu_timing_sample_count", gpuTimingSampleCount);
                    InsertJsonNumber(step, L"gpu_disjoint_count", gpuDisjointCount);
                    InsertJsonBool(step, L"gpu_timing_available", gpuTimingAvailable);
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"run_d3d11_fp32_timing_job\",\"feature_level\":" + JsonString(featureLevel) +
                        L",\"result_count\":" + std::to_wstring(resultCount) +
                        L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
                        L",\"total_bytes_timed\":" + std::to_wstring(totalBytesTimed) +
                        L",\"total_fp32_ops_timed\":" + std::to_wstring(totalFp32OpsTimed) +
                        L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
                        L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
                        L",\"gpu_timing_sample_count\":" + std::to_wstring(gpuTimingSampleCount) +
                        L",\"gpu_disjoint_count\":" + std::to_wstring(gpuDisjointCount) +
                        L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else if (action == L"run_d3d11_compute_timing_job")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    auto resultCount = GetOptionalUInt64(result, L"result_count");
                    auto totalElementsTimed = GetOptionalUInt64(result, L"total_elements_timed");
                    auto totalBytesTimed = GetOptionalUInt64(result, L"total_bytes_timed");
                    auto aggregateHash32 = GetOptionalUInt64(result, L"aggregate_hash32");
                    auto maxMismatchCount = GetOptionalUInt64(result, L"max_mismatch_count");
                    auto gpuTimingSampleCount = GetOptionalUInt64(result, L"gpu_timing_sample_count");
                    auto gpuDisjointCount = GetOptionalUInt64(result, L"gpu_disjoint_count");
                    auto featureLevel = GetOptionalString(result, L"feature_level");
                    auto gpuTimingAvailable = result.GetNamedBoolean(L"gpu_timing_available", false);
                    if (!verified || maxMismatchCount != 0)
                    {
                        throw WorkerTaskPlanError("task_plan.verify_failed", "D3D11 compute timing task step verification failed");
                    }
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"detail", L"D3D11 compute timing verified");
                    InsertJsonString(step, L"result_status", L"succeeded");
                    InsertJsonString(step, L"feature_level", featureLevel);
                    InsertJsonNumber(step, L"result_count", resultCount);
                    InsertJsonNumber(step, L"elements", totalElementsTimed);
                    InsertJsonNumber(step, L"bytes", totalBytesTimed);
                    InsertJsonNumber(step, L"aggregate_hash32", aggregateHash32);
                    InsertJsonNumber(step, L"max_mismatch_count", maxMismatchCount);
                    InsertJsonNumber(step, L"gpu_timing_sample_count", gpuTimingSampleCount);
                    InsertJsonNumber(step, L"gpu_disjoint_count", gpuDisjointCount);
                    InsertJsonBool(step, L"gpu_timing_available", gpuTimingAvailable);
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"run_d3d11_compute_timing_job\",\"feature_level\":" + JsonString(featureLevel) +
                        L",\"result_count\":" + std::to_wstring(resultCount) +
                        L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
                        L",\"total_bytes_timed\":" + std::to_wstring(totalBytesTimed) +
                        L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
                        L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
                        L",\"gpu_timing_sample_count\":" + std::to_wstring(gpuTimingSampleCount) +
                        L",\"gpu_disjoint_count\":" + std::to_wstring(gpuDisjointCount) +
                        L",\"gpu_timing_available\":" + BoolJson(gpuTimingAvailable) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else if (action == L"run_d3d11_compute_sweep_job")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    auto resultCount = GetOptionalUInt64(result, L"result_count");
                    auto totalElementsProcessed = GetOptionalUInt64(result, L"total_elements_processed");
                    auto totalBytesProcessed = GetOptionalUInt64(result, L"total_bytes_processed");
                    auto aggregateHash32 = GetOptionalUInt64(result, L"aggregate_hash32");
                    auto maxMismatchCount = GetOptionalUInt64(result, L"max_mismatch_count");
                    auto featureLevel = GetOptionalString(result, L"feature_level");
                    if (!verified || maxMismatchCount != 0)
                    {
                        throw WorkerTaskPlanError("task_plan.verify_failed", "D3D11 compute sweep task step verification failed");
                    }
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"detail", L"D3D11 compute sweep verified");
                    InsertJsonString(step, L"result_status", L"succeeded");
                    InsertJsonString(step, L"feature_level", featureLevel);
                    InsertJsonNumber(step, L"result_count", resultCount);
                    InsertJsonNumber(step, L"elements", totalElementsProcessed);
                    InsertJsonNumber(step, L"bytes", totalBytesProcessed);
                    InsertJsonNumber(step, L"aggregate_hash32", aggregateHash32);
                    InsertJsonNumber(step, L"max_mismatch_count", maxMismatchCount);
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"run_d3d11_compute_sweep_job\",\"feature_level\":" + JsonString(featureLevel) +
                        L",\"result_count\":" + std::to_wstring(resultCount) +
                        L",\"total_elements_processed\":" + std::to_wstring(totalElementsProcessed) +
                        L",\"total_bytes_processed\":" + std::to_wstring(totalBytesProcessed) +
                        L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
                        L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else if (action == L"run_d3d11_compute_job")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    auto elements = GetOptionalUInt64(result, L"elements");
                    auto requestedElements = GetOptionalUInt64(result, L"requested_elements");
                    auto dispatchGroups = GetOptionalUInt64(result, L"dispatch_groups");
                    auto bytes = GetOptionalUInt64(result, L"bytes");
                    auto hash32 = GetOptionalUInt64(result, L"hash32");
                    auto mismatches = GetOptionalUInt64(result, L"mismatch_count");
                    auto shaderBytes = GetOptionalUInt64(result, L"shader_bytes");
                    auto featureLevel = GetOptionalString(result, L"feature_level");
                    if (!verified)
                    {
                        throw WorkerTaskPlanError("task_plan.verify_failed", "D3D11 compute task step verification failed");
                    }
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"detail", L"D3D11 compute dispatch verified");
                    InsertJsonString(step, L"result_status", L"succeeded");
                    InsertJsonString(step, L"feature_level", featureLevel);
                    InsertJsonNumber(step, L"elements", elements);
                    InsertJsonNumber(step, L"requested_elements", requestedElements);
                    InsertJsonNumber(step, L"dispatch_groups", dispatchGroups);
                    InsertJsonNumber(step, L"bytes", bytes);
                    InsertJsonNumber(step, L"hash32", hash32);
                    InsertJsonNumber(step, L"mismatch_count", mismatches);
                    InsertJsonNumber(step, L"shader_bytes", shaderBytes);
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"run_d3d11_compute_job\",\"feature_level\":" + JsonString(featureLevel) +
                        L",\"requested_elements\":" + std::to_wstring(requestedElements) +
                        L",\"elements\":" + std::to_wstring(elements) +
                        L",\"dispatch_groups\":" + std::to_wstring(dispatchGroups) +
                        L",\"bytes\":" + std::to_wstring(bytes) +
                        L",\"hash32\":" + std::to_wstring(hash32) +
                        L",\"mismatch_count\":" + std::to_wstring(mismatches) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else if (action == L"store_memory_program")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto programId = GetOptionalString(result, L"program_id");
                    auto path = GetOptionalString(result, L"path");
                    auto bytecodeBytes = GetOptionalUInt64(result, L"bytecode_bytes");
                    auto bytecodeHash = GetOptionalUInt64(result, L"bytecode_hash32");
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"program_id", programId);
                    InsertJsonString(step, L"path", path);
                    InsertJsonNumber(step, L"bytes", bytecodeBytes);
                    InsertJsonNumber(step, L"expected_bytecode_hash32", bytecodeHash);
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"store_memory_program\",\"program_id\":" + JsonString(programId) +
                        L",\"path\":" + JsonString(path) +
                        L",\"bytecode_bytes\":" + std::to_wstring(bytecodeBytes) +
                        L",\"bytecode_hash32\":" + std::to_wstring(bytecodeHash) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else if (action == L"submit_stored_memory_file_job")
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto jobId = GetOptionalString(result, L"job_id");
                    if (jobId.empty())
                    {
                        throw WorkerTaskPlanError("job_id.required", "submit_stored_memory_file_job did not return job_id");
                    }
                    auto status = GetOptionalString(result, L"status", L"queued");
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonString(step, L"job_id", jobId);
                    InsertJsonString(step, L"async_job_id", jobId);
                    InsertJsonString(step, L"result_status", status);
                    InsertJsonBool(step, L"verified", true);
                    steps.SetAt(static_cast<uint32_t>(stepIndex), step);
                    for (uint32_t i = static_cast<uint32_t>(stepIndex + 1); i < steps.Size(); ++i)
                    {
                        auto future = steps.GetObjectAt(i);
                        auto futureAction = GetOptionalString(future, L"action");
                        if ((futureAction == L"get_job_status" || futureAction == L"get_job_result" || futureAction == L"purge_job") &&
                            GetOptionalString(future, L"job_id").empty())
                        {
                            InsertJsonString(future, L"job_id", jobId);
                            InsertJsonString(future, L"async_job_id", jobId);
                            steps.SetAt(i, future);
                        }
                    }
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"submit_stored_memory_file_job\",\"job_id\":" + JsonString(jobId) +
                        L",\"status\":" + JsonString(status) +
                        L",\"verified\":true" +
                        L"}";
                }
                else if (action == L"get_job_status")
                {
                    auto jobId = GetOptionalString(step, L"job_id", GetOptionalString(step, L"async_job_id"));
                    if (jobId.empty())
                    {
                        throw WorkerTaskPlanError("job_id.required", "get_job_status task step requires job_id");
                    }
                    InsertJsonString(step, L"job_id", jobId);
                    InsertJsonString(step, L"async_job_id", jobId);
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto jobStatus = result.GetNamedObject(L"job_status");
                    auto status = GetOptionalString(jobStatus, L"status");
                    auto completed = jobStatus.GetNamedBoolean(L"completed", false);
                    auto resultAvailable = jobStatus.GetNamedBoolean(L"result_available", false);
                    InsertJsonString(step, L"result_status", status);
                    InsertJsonBool(step, L"result_available", resultAvailable);
                    if (!completed)
                    {
                        InsertJsonString(step, L"status", L"pending");
                        InsertJsonString(step, L"detail", L"async job is not complete");
                        completedStep = false;
                        blocked = true;
                        blockedReason = L"job_not_complete";
                    }
                    else
                    {
                        InsertJsonString(step, L"status", L"completed");
                        InsertJsonString(step, L"detail", L"async job completed");
                        InsertJsonBool(step, L"verified", true);
                    }
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"get_job_status\",\"job_id\":" + JsonString(jobId) +
                        L",\"status\":" + JsonString(status) +
                        L",\"completed\":" + BoolJson(completed) +
                        L",\"result_available\":" + BoolJson(resultAvailable) +
                        L"}";
                }
                else if (action == L"get_job_result")
                {
                    auto jobId = GetOptionalString(step, L"job_id", GetOptionalString(step, L"async_job_id"));
                    if (jobId.empty())
                    {
                        throw WorkerTaskPlanError("job_id.required", "get_job_result task step requires job_id");
                    }
                    InsertJsonString(step, L"job_id", jobId);
                    InsertJsonString(step, L"async_job_id", jobId);
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto status = GetOptionalString(result, L"status");
                    auto completed = result.GetNamedBoolean(L"completed", false);
                    auto resultAvailable = result.GetNamedBoolean(L"result_available", false);
                    auto persisted = result.GetNamedBoolean(L"persisted", false);
                    InsertJsonString(step, L"result_status", status);
                    InsertJsonBool(step, L"result_available", resultAvailable);
                    if (!completed || !resultAvailable)
                    {
                        InsertJsonString(step, L"status", L"pending");
                        InsertJsonString(step, L"detail", L"async job result is not available");
                        completedStep = false;
                        blocked = true;
                        blockedReason = L"result_not_available";
                    }
                    else
                    {
                        auto resultObject = result.GetNamedObject(L"result");
                        auto outputSha256 = GetOptionalString(resultObject, L"output_sha256");
                        auto expectedOutputSha256 = GetOptionalString(step, L"expected_output_sha256");
                        auto verified = expectedOutputSha256.empty() || expectedOutputSha256 == outputSha256;
                        if (step.HasKey(L"expected_persisted"))
                        {
                            verified = verified && (persisted == step.GetNamedBoolean(L"expected_persisted", false));
                        }
                        if (!verified)
                        {
                            throw WorkerTaskPlanError("task_plan.verify_failed", "get_job_result task step verification failed");
                        }
                        InsertJsonString(step, L"status", L"completed");
                        InsertJsonString(step, L"detail", L"async job result available");
                        InsertJsonString(step, L"output_sha256", outputSha256);
                        InsertJsonBool(step, L"verified", verified);
                        if (!outputSha256.empty())
                        {
                            for (uint32_t i = static_cast<uint32_t>(stepIndex + 1); i < steps.Size(); ++i)
                            {
                                auto future = steps.GetObjectAt(i);
                                if (GetOptionalString(future, L"action") == L"get_job_result" &&
                                    GetOptionalString(future, L"expected_output_sha256").empty())
                                {
                                    InsertJsonString(future, L"expected_output_sha256", outputSha256);
                                    steps.SetAt(i, future);
                                }
                            }
                        }
                    }
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"get_job_result\",\"job_id\":" + JsonString(jobId) +
                        L",\"status\":" + JsonString(status) +
                        L",\"completed\":" + BoolJson(completed) +
                        L",\"result_available\":" + BoolJson(resultAvailable) +
                        L",\"persisted\":" + BoolJson(persisted);
                    if (result.HasKey(L"result") && result.GetNamedObject(L"result").HasKey(L"output_sha256"))
                    {
                        resultJson += L",\"output_sha256\":" + JsonString(GetOptionalString(result.GetNamedObject(L"result"), L"output_sha256"));
                    }
                    resultJson += L"}";
                }
                else if (action == L"purge_job")
                {
                    auto jobId = GetOptionalString(step, L"job_id", GetOptionalString(step, L"async_job_id"));
                    if (jobId.empty())
                    {
                        throw WorkerTaskPlanError("job_id.required", "purge_job task step requires job_id");
                    }
                    InsertJsonString(step, L"job_id", jobId);
                    InsertJsonString(step, L"async_job_id", jobId);
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto preserveSnapshot = result.GetNamedBoolean(L"preserve_snapshot", false);
                    auto persistedRemoved = result.GetNamedBoolean(L"persisted_removed", false);
                    auto memoryExisted = result.GetNamedBoolean(L"memory_existed", false);
                    auto persistedExisted = result.GetNamedBoolean(L"persisted_existed", false);
                    auto verified = preserveSnapshot ? !persistedRemoved : (persistedRemoved || !persistedExisted);
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonBool(step, L"verified", verified);
                    resultJson = L"{\"step_id\":" + JsonString(stepId) +
                        L",\"action\":\"purge_job\",\"job_id\":" + JsonString(jobId) +
                        L",\"memory_existed\":" + BoolJson(memoryExisted) +
                        L",\"persisted_existed\":" + BoolJson(persistedExisted) +
                        L",\"preserve_snapshot\":" + BoolJson(preserveSnapshot) +
                        L",\"persisted_removed\":" + BoolJson(persistedRemoved) +
                        L",\"verified\":" + BoolJson(verified) +
                        L"}";
                }
                else
                {
                    auto result = JsonObject::Parse(ExecuteAction(action, step, root));
                    auto verified = result.GetNamedBoolean(L"verified", false);
                    InsertJsonString(step, L"status", L"completed");
                    InsertJsonBool(step, L"verified", verified);
                    if (action == L"write_chunk")
                    {
                        auto bytesWritten = GetOptionalUInt64(result, L"bytes_written");
                        auto offset = GetOptionalUInt64(result, L"offset");
                        InsertJsonNumber(step, L"bytes", bytesWritten);
                        InsertJsonNumber(step, L"chunk_offset", offset);
                        InsertJsonNumber(step, L"chunk_bytes", bytesWritten);
                    }
                    else if (action == L"read_chunk")
                    {
                        InsertJsonNumber(step, L"bytes", GetOptionalUInt64(result, L"bytes_read"));
                    }
                    else if (action == L"hash_file")
                    {
                        InsertJsonString(step, L"sha256", GetOptionalString(result, L"sha256"));
                        InsertJsonNumber(step, L"bytes", GetOptionalUInt64(result, L"bytes"));
                    }
                    else if (action == L"delete")
                    {
                        InsertJsonNumber(step, L"bytes", GetOptionalUInt64(result, L"removed"));
                    }
                    resultJson = std::wstring(result.Stringify().c_str());
                }
                steps.SetAt(static_cast<uint32_t>(stepIndex), step);
                RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
                PersistTaskPlan(planPath, plan);
                stepResults.push_back(resultJson);
                if (!completedStep)
                {
                    break;
                }
                ++advancedCount;
            }
            catch (WorkerTaskPlanError const& error)
            {
                auto failedStep = steps.GetObjectAt(static_cast<uint32_t>(stepIndex));
                InsertJsonString(failedStep, L"status", L"failed");
                InsertJsonNumber(failedStep, L"failures", GetOptionalUInt64(failedStep, L"failures", 0) + 1);
                InsertJsonString(failedStep, L"detail", Utf8ToWide(error.message));
                steps.SetAt(static_cast<uint32_t>(stepIndex), failedStep);
                RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
                PersistTaskPlan(planPath, plan);
                throw;
            }
        }

        auto finalCounts = RefreshTaskPlanMetadata(plan, steps, protocolVersion_);
        PersistTaskPlan(planPath, plan);
        auto complete = finalCounts.completed == steps.Size() && finalCounts.failed == 0 && finalCounts.running == 0 && finalCounts.pending == 0;
        int32_t nextStepIndex = -1;
        std::wstring nextStepId;
        std::wstring nextStepStatus;
        std::wstring nextAction;
        for (uint32_t i = 0; i < steps.Size(); ++i)
        {
            auto step = steps.GetObjectAt(i);
            auto status = GetOptionalString(step, L"status", L"pending");
            if (status == L"completed" || status == L"skipped")
            {
                continue;
            }
            nextStepIndex = static_cast<int32_t>(i);
            nextStepId = GetOptionalString(step, L"step_id");
            nextStepStatus = status;
            nextAction = GetOptionalString(step, L"action");
            break;
        }

        std::wostringstream resultsOut;
        resultsOut << L"[";
        for (size_t i = 0; i < stepResults.size(); ++i)
        {
            if (i != 0)
            {
                resultsOut << L",";
            }
            resultsOut << stepResults[i];
        }
        resultsOut << L"]";

        return OkBase(protocolVersion_, L"advance_task_plan") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"path\":" + JsonString(planPath.lexically_relative(root).wstring()) +
            L",\"max_steps\":" + std::to_wstring(maxSteps) +
            L",\"advanced_count\":" + std::to_wstring(advancedCount) +
            L",\"blocked\":" + BoolJson(blocked) +
            L",\"blocked_reason\":" + JsonString(blockedReason) +
            L",\"complete\":" + BoolJson(complete) +
            L",\"reset_running\":" + BoolJson(resetRunning) +
            L",\"running_reset_count\":" + std::to_wstring(runningResetCount) +
            L",\"task_status\":" + JsonString(finalCounts.status) +
            L",\"next_step_index\":" + std::to_wstring(nextStepIndex) +
            L",\"next_step_id\":" + JsonString(nextStepId) +
            L",\"next_step_status\":" + JsonString(nextStepStatus) +
            L",\"next_action\":" + JsonString(nextAction) +
            L",\"step_count\":" + std::to_wstring(steps.Size()) +
            L",\"pending_count\":" + std::to_wstring(finalCounts.pending) +
            L",\"running_count\":" + std::to_wstring(finalCounts.running) +
            L",\"completed_count\":" + std::to_wstring(finalCounts.completed) +
            L",\"failed_count\":" + std::to_wstring(finalCounts.failed) +
            L",\"step_results\":" + resultsOut.str() +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    void WorkerTaskPlanRuntime::WriteTaskPlanRunProgress(fs::path const& root, std::wstring const& taskId, TaskPlanRunProgress const& progress)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }

        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        JsonObject autoRun;
        InsertJsonString(autoRun, L"schema_version", RunControlSchemaVersion());
        InsertJsonString(autoRun, L"run_id", progress.runId);
        InsertJsonString(autoRun, L"status", progress.status);
        InsertJsonString(autoRun, L"last_blocked_reason", progress.lastBlockedReason);
        InsertJsonString(autoRun, L"last_next_step_id", progress.lastNextStepId);
        InsertJsonString(autoRun, L"output_sha256", progress.outputSha256);
        InsertJsonString(autoRun, L"error_code", progress.errorCode);
        InsertJsonString(autoRun, L"error_message", progress.errorMessage);
        InsertJsonNumber(autoRun, L"cycle_count", progress.cycleCount);
        InsertJsonNumber(autoRun, L"total_advanced_count", progress.totalAdvancedCount);
        InsertJsonNumber(autoRun, L"blocked_poll_count", progress.blockedPollCount);
        InsertJsonBool(autoRun, L"complete", progress.complete);
        InsertJsonBool(autoRun, L"result_available", progress.resultAvailable);
        InsertJsonBool(autoRun, L"cancel_requested", progress.cancelRequested);
        plan.Insert(L"auto_run", autoRun);
        InsertJsonString(plan, L"schema_version", TaskPlanSchemaVersion);
        InsertJsonString(plan, L"protocol_version", protocolVersion_);
        PersistTaskPlan(planPath, plan);

        std::lock_guard<std::mutex> runLock(runMutex_);
        latestTaskPlanId_ = taskId;
        latestRunId_ = progress.runId;
        latestRunStatus_ = progress.status;
        latestBlockedReason_ = progress.lastBlockedReason;
        if (!progress.errorCode.empty())
        {
            latestError_ = progress.errorCode;
        }
        else
        {
            latestError_ = progress.errorMessage;
        }
    }

    std::wstring WorkerTaskPlanRuntime::SubmitTaskPlanRun(
        JsonObject const& request,
        fs::path const& root)
    {
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto maxSteps = GetBoundedOptionalUInt64(request, L"max_steps", 16, 1, 16);
        auto pollIntervalMs = GetBoundedOptionalUInt64(request, L"poll_interval_ms", 100, 25, 5000);
        auto timeoutSeconds = GetBoundedOptionalUInt64(request, L"timeout_seconds", 30, 1, 3600);
        auto control = std::make_shared<TaskPlanRunControl>();
        control->runId = L"run-" + GenerateSessionId().substr(0, 16);

        std::wstring existingRunId;
        {
            std::lock_guard<std::mutex> runLock(runMutex_);
            auto found = activeRuns_.find(taskId);
            if (found != activeRuns_.end())
            {
                existingRunId = found->second->runId;
            }
            else
            {
                activeRuns_[taskId] = control;
            }
        }

        if (!existingRunId.empty())
        {
            return OkBase(protocolVersion_, L"submit_task_plan_run") +
                L",\"task_id\":" + JsonString(taskId) +
                L",\"run_id\":" + JsonString(existingRunId) +
                L",\"accepted\":false" +
                L",\"reason\":\"already_running\"" +
                L"}";
        }

        try
        {
            TaskPlanRunProgress progress;
            progress.runId = control->runId;
            progress.status = L"queued";
            WriteTaskPlanRunProgress(root, taskId, progress);
            std::thread([
                this,
                taskId,
                root,
                control,
                maxSteps,
                pollIntervalMs,
                timeoutSeconds]()
            {
                RunTaskPlanAutoRunner(
                    taskId,
                    root,
                    control,
                    maxSteps,
                    pollIntervalMs,
                    timeoutSeconds);
            }).detach();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> runLock(runMutex_);
            auto found = activeRuns_.find(taskId);
            if (found != activeRuns_.end() && found->second == control)
            {
                activeRuns_.erase(found);
            }
            throw;
        }

        return OkBase(protocolVersion_, L"submit_task_plan_run") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"run_id\":" + JsonString(control->runId) +
            L",\"accepted\":true" +
            L",\"max_steps\":" + std::to_wstring(maxSteps) +
            L",\"poll_interval_ms\":" + std::to_wstring(pollIntervalMs) +
            L",\"timeout_seconds\":" + std::to_wstring(timeoutSeconds) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::GetTaskPlanStatus(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }
        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        auto steps = plan.GetNamedArray(L"steps");
        auto counts = CountTaskPlanSteps(steps);
        auto complete = counts.completed == steps.Size() && counts.failed == 0 && counts.running == 0 && counts.pending == 0;
        auto autoRunJson = TaskPlanAutoRunJson(plan);
        std::wstring autoRunStatus;
        std::wstring runId;
        bool resultAvailable = complete;
        if (plan.HasKey(L"auto_run"))
        {
            auto autoRun = plan.GetNamedObject(L"auto_run");
            autoRunStatus = GetOptionalString(autoRun, L"status");
            runId = GetOptionalString(autoRun, L"run_id");
            resultAvailable = autoRun.GetNamedBoolean(L"result_available", false);
        }
        auto terminal = complete || counts.failed > 0 ||
            autoRunStatus == L"completed" || autoRunStatus == L"blocked" ||
            autoRunStatus == L"failed" || autoRunStatus == L"canceled";
        return OkBase(protocolVersion_, L"get_task_plan_status") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"run_id\":" + JsonString(runId) +
            L",\"task_status\":" + JsonString(counts.status) +
            L",\"auto_run_status\":" + JsonString(autoRunStatus) +
            L",\"complete\":" + BoolJson(complete) +
            L",\"terminal\":" + BoolJson(terminal) +
            L",\"result_available\":" + BoolJson(resultAvailable) +
            L",\"step_count\":" + std::to_wstring(steps.Size()) +
            L",\"pending_count\":" + std::to_wstring(counts.pending) +
            L",\"running_count\":" + std::to_wstring(counts.running) +
            L",\"completed_count\":" + std::to_wstring(counts.completed) +
            L",\"failed_count\":" + std::to_wstring(counts.failed) +
            L",\"auto_run\":" + autoRunJson +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::GetTaskPlanResult(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }
        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        auto steps = plan.GetNamedArray(L"steps");
        auto counts = CountTaskPlanSteps(steps);
        auto complete = counts.completed == steps.Size() && counts.failed == 0 && counts.running == 0 && counts.pending == 0;
        auto outputSha256 = FindTaskPlanOutputSha256(plan);
        auto resultAvailable = complete;
        std::wstring autoRunStatus;
        std::wstring runId;
        if (plan.HasKey(L"auto_run"))
        {
            auto autoRun = plan.GetNamedObject(L"auto_run");
            autoRunStatus = GetOptionalString(autoRun, L"status");
            runId = GetOptionalString(autoRun, L"run_id");
            resultAvailable = resultAvailable || autoRun.GetNamedBoolean(L"result_available", false);
        }

        return OkBase(protocolVersion_, L"get_task_plan_result") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"run_id\":" + JsonString(runId) +
            L",\"task_status\":" + JsonString(counts.status) +
            L",\"auto_run_status\":" + JsonString(autoRunStatus) +
            L",\"complete\":" + BoolJson(complete) +
            L",\"result_available\":" + BoolJson(resultAvailable) +
            L",\"output_sha256\":" + JsonString(outputSha256) +
            L",\"step_count\":" + std::to_wstring(steps.Size()) +
            L",\"completed_count\":" + std::to_wstring(counts.completed) +
            L",\"failed_count\":" + std::to_wstring(counts.failed) +
            L",\"auto_run\":" + TaskPlanAutoRunJson(plan) +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    void WorkerTaskPlanRuntime::RunTaskPlanAutoRunner(
        std::wstring taskId,
        fs::path root,
        std::shared_ptr<TaskPlanRunControl> control,
        uint64_t maxSteps,
        uint64_t pollIntervalMs,
        uint64_t timeoutSeconds)
    {
        TaskPlanRunProgress progress;
        progress.runId = control->runId;
        progress.status = L"running";
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

        auto persistCanceled = [&]()
        {
            progress.status = L"canceled";
            progress.cancelRequested = true;
            progress.complete = false;
            progress.resultAvailable = false;
            progress.errorCode = L"task_plan_run.canceled";
            progress.errorMessage = L"task plan auto-runner was canceled";
            WriteTaskPlanRunProgress(root, taskId, progress);
        };

        try
        {
            if (control->cancelRequested.load())
            {
                persistCanceled();
            }
            else
            {
                WriteTaskPlanRunProgress(root, taskId, progress);
                while (true)
                {
                    if (control->cancelRequested.load())
                    {
                        persistCanceled();
                        break;
                    }

                    JsonObject advanceRequest;
                    InsertJsonString(advanceRequest, L"task_id", taskId);
                    InsertJsonNumber(advanceRequest, L"max_steps", maxSteps);
                    InsertJsonBool(advanceRequest, L"reset_running", true);
                    auto advance = JsonObject::Parse(AdvanceTaskPlan(advanceRequest, root));
                    ++progress.cycleCount;
                    progress.totalAdvancedCount += GetOptionalUInt64(advance, L"advanced_count");
                    progress.complete = advance.GetNamedBoolean(L"complete", false);
                    progress.lastBlockedReason = GetOptionalString(advance, L"blocked_reason");
                    progress.lastNextStepId = GetOptionalString(advance, L"next_step_id");
                    if (advance.HasKey(L"task_plan"))
                    {
                        auto plan = advance.GetNamedObject(L"task_plan");
                        progress.outputSha256 = FindTaskPlanOutputSha256(plan);
                        progress.resultAvailable = progress.complete;
                    }

                    if (control->cancelRequested.load())
                    {
                        persistCanceled();
                        break;
                    }
                    if (progress.complete)
                    {
                        progress.status = L"completed";
                        progress.resultAvailable = true;
                        WriteTaskPlanRunProgress(root, taskId, progress);
                        break;
                    }

                    auto blocked = advance.GetNamedBoolean(L"blocked", false);
                    if (blocked)
                    {
                        if (progress.lastBlockedReason == L"job_not_complete" ||
                            progress.lastBlockedReason == L"result_not_available")
                        {
                            ++progress.blockedPollCount;
                            progress.status = L"running";
                            WriteTaskPlanRunProgress(root, taskId, progress);
                        }
                        else
                        {
                            progress.status = L"blocked";
                            WriteTaskPlanRunProgress(root, taskId, progress);
                            break;
                        }
                    }
                    else
                    {
                        progress.status = L"running";
                        WriteTaskPlanRunProgress(root, taskId, progress);
                    }

                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        progress.status = L"failed";
                        progress.errorCode = L"task_plan_run.timeout";
                        progress.errorMessage = L"task plan auto-runner timed out";
                        WriteTaskPlanRunProgress(root, taskId, progress);
                        break;
                    }

                    uint64_t sleptMs = 0;
                    while (sleptMs < pollIntervalMs && !control->cancelRequested.load())
                    {
                        auto sliceMs = (std::min)(uint64_t{ 25 }, pollIntervalMs - sleptMs);
                        std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
                        sleptMs += sliceMs;
                    }
                }
            }
        }
        catch (WorkerTaskPlanError const& ex)
        {
            if (control->cancelRequested.load())
            {
                try { persistCanceled(); } catch (...) {}
            }
            else
            {
                progress.status = L"failed";
                progress.errorCode = Utf8ToWide(ex.code);
                progress.errorMessage = Utf8ToWide(ex.message);
                try { WriteTaskPlanRunProgress(root, taskId, progress); } catch (...) {}
            }
        }
        catch (std::exception const& ex)
        {
            progress.status = L"failed";
            progress.errorCode = L"task_plan_run.exception";
            progress.errorMessage = Utf8ToWide(ex.what());
            try { WriteTaskPlanRunProgress(root, taskId, progress); } catch (...) {}
        }
        catch (...)
        {
            progress.status = L"failed";
            progress.errorCode = L"task_plan_run.unhandled";
            progress.errorMessage = L"unhandled task plan auto-runner failure";
            try { WriteTaskPlanRunProgress(root, taskId, progress); } catch (...) {}
        }

        std::lock_guard<std::mutex> runLock(runMutex_);
        auto found = activeRuns_.find(taskId);
        if (found != activeRuns_.end() && found->second == control)
        {
            activeRuns_.erase(found);
        }
    }

    std::wstring WorkerTaskPlanRuntime::UpdateTaskPlanStep(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        auto stepId = GetOptionalString(request, L"step_id");
        auto status = GetOptionalString(request, L"status");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        if (!IsSafeProgramId(stepId))
        {
            throw WorkerTaskPlanError("task_plan.step_id_invalid", "step_id must be 1..64 chars using letters, digits, underscore, or dash");
        }
        if (!IsTaskPlanStepStatus(status))
        {
            throw WorkerTaskPlanError("task_plan.step_status_invalid", "step status is invalid");
        }

        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }
        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        auto steps = plan.GetNamedArray(L"steps");
        bool updated = false;
        for (uint32_t i = 0; i < steps.Size(); ++i)
        {
            auto step = steps.GetObjectAt(i);
            if (GetOptionalString(step, L"step_id") != stepId)
            {
                continue;
            }

            InsertJsonString(step, L"status", status);
            if (request.HasKey(L"detail"))
            {
                auto detail = GetOptionalString(request, L"detail");
                if (detail.size() > 512)
                {
                    throw WorkerTaskPlanError("task_plan.step_detail_too_large", "step detail cannot exceed 512 chars");
                }
                InsertJsonString(step, L"detail", detail);
            }
            if (request.HasKey(L"async_job_id"))
            {
                InsertJsonString(step, L"async_job_id", GetOptionalString(request, L"async_job_id"));
            }
            if (request.HasKey(L"job_id"))
            {
                InsertJsonString(step, L"job_id", GetOptionalString(request, L"job_id"));
            }
            if (request.HasKey(L"path"))
            {
                InsertJsonString(step, L"path", GetOptionalString(request, L"path"));
            }
            if (request.HasKey(L"snapshot_path"))
            {
                InsertJsonString(step, L"snapshot_path", GetOptionalString(request, L"snapshot_path"));
            }
            if (request.HasKey(L"result_status"))
            {
                InsertJsonString(step, L"result_status", GetOptionalString(request, L"result_status"));
            }
            if (request.HasKey(L"bytes"))
            {
                InsertJsonNumber(step, L"bytes", GetOptionalUInt64(request, L"bytes"));
            }
            if (request.HasKey(L"chunk_offset"))
            {
                InsertJsonNumber(step, L"chunk_offset", GetOptionalUInt64(request, L"chunk_offset"));
            }
            if (request.HasKey(L"chunk_bytes"))
            {
                InsertJsonNumber(step, L"chunk_bytes", GetOptionalUInt64(request, L"chunk_bytes"));
            }
            if (request.HasKey(L"result_available"))
            {
                InsertJsonBool(step, L"result_available", request.GetNamedBoolean(L"result_available", false));
            }
            if (request.HasKey(L"verified"))
            {
                InsertJsonBool(step, L"verified", request.GetNamedBoolean(L"verified", false));
            }

            auto incrementAttempt = request.HasKey(L"increment_attempt")
                ? request.GetNamedBoolean(L"increment_attempt", false)
                : status == L"running";
            if (incrementAttempt)
            {
                InsertJsonNumber(step, L"attempts", GetOptionalUInt64(step, L"attempts", 0) + 1);
            }
            if (status == L"failed")
            {
                InsertJsonNumber(step, L"failures", GetOptionalUInt64(step, L"failures", 0) + 1);
            }
            steps.SetAt(i, step);
            updated = true;
            break;
        }
        if (!updated)
        {
            throw WorkerTaskPlanError("task_plan.step_not_found", "task plan step was not found");
        }

        uint64_t completedCount = 0;
        uint64_t failedCount = 0;
        uint64_t runningCount = 0;
        for (uint32_t i = 0; i < steps.Size(); ++i)
        {
            auto stepStatus = GetOptionalString(steps.GetObjectAt(i), L"status", L"pending");
            if (stepStatus == L"completed" || stepStatus == L"skipped")
            {
                ++completedCount;
            }
            else if (stepStatus == L"failed")
            {
                ++failedCount;
            }
            else if (stepStatus == L"running")
            {
                ++runningCount;
            }
        }

        auto planStatus = failedCount > 0
            ? L"failed"
            : (runningCount > 0 ? L"running" : (completedCount == steps.Size() ? L"completed" : L"pending"));
        InsertJsonString(plan, L"protocol_version", protocolVersion_);
        InsertJsonString(plan, L"status", planStatus);
        InsertJsonNumber(plan, L"step_count", steps.Size());
        InsertJsonNumber(plan, L"completed_count", completedCount);
        InsertJsonNumber(plan, L"failed_count", failedCount);
        InsertJsonNumber(plan, L"running_count", runningCount);
        WriteTextFile(planPath, WideToUtf8(std::wstring(plan.Stringify().c_str())));

        return OkBase(protocolVersion_, L"update_task_plan_step") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"step_id\":" + JsonString(stepId) +
            L",\"status\":" + JsonString(status) +
            L",\"task_status\":" + JsonString(planStatus) +
            L",\"completed_count\":" + std::to_wstring(completedCount) +
            L",\"failed_count\":" + std::to_wstring(failedCount) +
            L",\"task_plan\":" + std::wstring(plan.Stringify().c_str()) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::DeleteTaskPlan(JsonObject const& request, fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto taskId = GetOptionalString(request, L"task_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }
        auto planPath = ResolveTaskPlanPath(root, taskId);
        auto existed = fs::is_regular_file(planPath);
        auto removed = existed && fs::remove(planPath);
        return OkBase(protocolVersion_, L"delete_task_plan") +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"path\":" + JsonString(planPath.lexically_relative(root).wstring()) +
            L",\"existed\":" + BoolJson(existed) +
            L",\"removed\":" + BoolJson(removed) +
            L"}";
    }

    std::wstring WorkerTaskPlanRuntime::ListTaskPlans(fs::path const& root)
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        std::vector<std::wstring> entries;
        auto directory = TaskPlanDirectory(root);
        if (fs::exists(directory))
        {
            for (auto const& entry : fs::directory_iterator(directory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".json")
                {
                    continue;
                }
                try
                {
                    auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(entry.path())));
                    entries.push_back(TaskPlanSummaryJson(plan));
                }
                catch (...)
                {
                }
            }
        }
        std::sort(entries.begin(), entries.end());

        std::wostringstream plans;
        plans << L"[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i != 0)
            {
                plans << L",";
            }
            plans << entries[i];
        }
        plans << L"]";

        return OkBase(protocolVersion_, L"list_task_plans") +
            L",\"task_plan_count\":" + std::to_wstring(entries.size()) +
            L",\"task_plans\":" + plans.str() +
            L"}";
    }


    uint64_t WorkerTaskPlanRuntime::CountTaskPlans(fs::path const& root) const
    {
        std::lock_guard<std::mutex> taskLock(planMutex_);
        uint64_t count = 0;
        auto directory = TaskPlanDirectory(root);
        if (!fs::exists(directory))
        {
            return 0;
        }
        for (auto const& entry : fs::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".json")
            {
                ++count;
            }
        }
        return count;
    }

    WorkerTaskPlanRuntimeStatus WorkerTaskPlanRuntime::Snapshot() const
    {
        std::lock_guard<std::mutex> runLock(runMutex_);
        WorkerTaskPlanRuntimeStatus snapshot;
        snapshot.activeRunCount = activeRuns_.size();
        if (!activeRuns_.empty())
        {
            snapshot.activeTaskPlanId = activeRuns_.begin()->first;
            snapshot.activeRunId = activeRuns_.begin()->second->runId;
        }
        snapshot.latestTaskPlanId = latestTaskPlanId_;
        snapshot.latestRunId = latestRunId_;
        snapshot.latestRunStatus = latestRunStatus_;
        snapshot.latestBlockedReason = latestBlockedReason_;
        snapshot.latestError = latestError_;
        return snapshot;
    }

    std::wstring WorkerTaskPlanRuntime::CancelTaskPlanRun(
        JsonObject const& request,
        fs::path const& root)
    {
        auto taskId = GetOptionalString(request, L"task_id");
        auto requestedRunId = GetOptionalString(request, L"run_id");
        if (taskId.empty())
        {
            throw WorkerTaskPlanError("task_id.required", "task_id is required");
        }

        std::shared_ptr<TaskPlanRunControl> control;
        {
            std::lock_guard<std::mutex> runLock(runMutex_);
            auto found = activeRuns_.find(taskId);
            if (found != activeRuns_.end())
            {
                control = found->second;
                if (!requestedRunId.empty() && requestedRunId != control->runId)
                {
                    throw WorkerTaskPlanError("task_plan_run.run_id_mismatch", "run_id does not match the active task-plan run");
                }
            }
        }

        if (control)
        {
            auto accepted = !control->cancelRequested.exchange(true);
            {
                std::lock_guard<std::mutex> runLock(runMutex_);
                latestTaskPlanId_ = taskId;
                latestRunId_ = control->runId;
                latestRunStatus_ = L"canceling";
                latestBlockedReason_.clear();
                latestError_.clear();
            }
            return OkBase(protocolVersion_, L"cancel_task_plan_run") +
                L",\"schema_version\":" + JsonString(RunControlSchemaVersion()) +
                L",\"task_id\":" + JsonString(taskId) +
                L",\"run_id\":" + JsonString(control->runId) +
                L",\"status\":\"canceling\"" +
                L",\"cancel_requested\":true" +
                L",\"cancel_accepted\":" + BoolJson(accepted) +
                L"}";
        }

        std::lock_guard<std::mutex> taskLock(planMutex_);
        auto planPath = ResolveTaskPlanPath(root, taskId);
        if (!fs::is_regular_file(planPath))
        {
            throw WorkerTaskPlanError("task_plan.not_found", "task plan was not found");
        }
        auto plan = JsonObject::Parse(Utf8ToWide(ReadTextFile(planPath)));
        if (!plan.HasKey(L"auto_run"))
        {
            throw WorkerTaskPlanError("task_plan_run.not_found", "task plan has no run state");
        }
        auto autoRun = plan.GetNamedObject(L"auto_run");
        auto runId = GetOptionalString(autoRun, L"run_id");
        auto status = GetOptionalString(autoRun, L"status");
        if (!requestedRunId.empty() && requestedRunId != runId)
        {
            throw WorkerTaskPlanError("task_plan_run.run_id_mismatch", "run_id does not match the task-plan run");
        }
        return OkBase(protocolVersion_, L"cancel_task_plan_run") +
            L",\"schema_version\":" + JsonString(RunControlSchemaVersion()) +
            L",\"task_id\":" + JsonString(taskId) +
            L",\"run_id\":" + JsonString(runId) +
            L",\"status\":" + JsonString(status) +
            L",\"cancel_requested\":" + BoolJson(status == L"canceled") +
            L",\"cancel_accepted\":false" +
            L"}";
    }
}