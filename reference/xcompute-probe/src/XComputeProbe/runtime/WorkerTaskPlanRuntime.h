#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerTaskPlanError : std::exception
    {
        std::string code;
        std::string message;

        WorkerTaskPlanError(std::string codeValue, std::string messageValue)
            : code(std::move(codeValue)), message(std::move(messageValue))
        {
        }

        char const* what() const noexcept override
        {
            return message.c_str();
        }
    };

    struct WorkerTaskPlanRuntimeStatus
    {
        size_t activeRunCount = 0;
        std::wstring activeTaskPlanId;
        std::wstring activeRunId;
        std::wstring latestTaskPlanId;
        std::wstring latestRunId;
        std::wstring latestRunStatus;
        std::wstring latestBlockedReason;
        std::wstring latestError;
    };

    using WorkerTaskPlanActionExecutor = std::function<std::wstring(
        std::wstring const& action,
        winrt::Windows::Data::Json::JsonObject const& step,
        std::filesystem::path const& root)>;

    class WorkerTaskPlanRuntime
    {
    public:
        WorkerTaskPlanRuntime(
            std::wstring protocolVersion,
            WorkerTaskPlanActionExecutor actionExecutor);

        static wchar_t const* SchemaVersion();
        static wchar_t const* RunControlSchemaVersion();
        static bool HandlesCommand(std::wstring const& command);

        std::wstring ExecuteCommand(
            std::wstring const& command,
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);

        std::wstring CreateTaskPlan(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring GetTaskPlan(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring ResumeTaskPlan(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring AdvanceTaskPlan(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring SubmitTaskPlanRun(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring CancelTaskPlanRun(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring GetTaskPlanStatus(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring GetTaskPlanResult(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring UpdateTaskPlanStep(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring DeleteTaskPlan(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring ListTaskPlans(std::filesystem::path const& root);

        uint64_t CountTaskPlans(std::filesystem::path const& root) const;
        WorkerTaskPlanRuntimeStatus Snapshot() const;

    private:
        struct TaskPlanRunProgress
        {
            std::wstring runId;
            std::wstring status;
            std::wstring lastBlockedReason;
            std::wstring lastNextStepId;
            std::wstring outputSha256;
            std::wstring errorCode;
            std::wstring errorMessage;
            uint64_t cycleCount = 0;
            uint64_t totalAdvancedCount = 0;
            uint64_t blockedPollCount = 0;
            bool complete = false;
            bool resultAvailable = false;
            bool cancelRequested = false;
        };

        struct TaskPlanRunControl
        {
            std::wstring runId;
            std::atomic_bool cancelRequested{ false };
        };

        std::wstring ExecuteAction(
            std::wstring const& action,
            winrt::Windows::Data::Json::JsonObject const& step,
            std::filesystem::path const& root) const;
        void WriteTaskPlanRunProgress(
            std::filesystem::path const& root,
            std::wstring const& taskId,
            TaskPlanRunProgress const& progress);
        void RunTaskPlanAutoRunner(
            std::wstring taskId,
            std::filesystem::path root,
            std::shared_ptr<TaskPlanRunControl> control,
            uint64_t maxSteps,
            uint64_t pollIntervalMs,
            uint64_t timeoutSeconds);

        std::wstring protocolVersion_;
        WorkerTaskPlanActionExecutor actionExecutor_;
        mutable std::mutex planMutex_;
        mutable std::mutex runMutex_;
        std::map<std::wstring, std::shared_ptr<TaskPlanRunControl>> activeRuns_;
        std::wstring latestTaskPlanId_;
        std::wstring latestRunId_;
        std::wstring latestRunStatus_;
        std::wstring latestBlockedReason_;
        std::wstring latestError_;
    };
}
