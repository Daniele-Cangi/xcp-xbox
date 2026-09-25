#include "pch.h"
#include "WorkerXvmCpuTieredRuntime.h"

namespace XComputeProbe
{
    namespace
    {
        bool IsHex(std::wstring const& value, size_t expectedLength)
        {
            if (value.size() != expectedLength)
            {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                    (ch >= L'A' && ch <= L'F');
            });
        }

        uint32_t HexWord(std::wstring const& value, size_t offset)
        {
            uint32_t result = 0;
            for (size_t index = 0; index < 8; ++index)
            {
                auto ch = value[offset + index];
                auto nibble = ch >= L'0' && ch <= L'9'
                    ? static_cast<uint32_t>(ch - L'0')
                    : static_cast<uint32_t>((ch | 0x20) - L'a' + 10);
                result = (result << 4) | nibble;
            }
            return result;
        }

        std::wstring DoubleJson(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << value;
            return out.str();
        }

        WorkerXvmCpuTieredDifferentialResult Canceled(
            WorkerXvmMachineState state,
            double referenceElapsedMs = 0.0)
        {
            WorkerXvmCpuTieredDifferentialResult result;
            result.canceled = true;
            result.canceledState = std::move(state);
            result.referenceElapsedMs = referenceElapsedMs;
            return result;
        }
    }

    std::array<uint32_t, WorkerXvmInputWordCountValue> WorkerBuildXvmInputWords(
        std::wstring const& boundInputSha256,
        std::wstring const& boundInputMix64,
        bool inputControlPass)
    {
        if (!IsHex(boundInputSha256, 64) || !IsHex(boundInputMix64, 16))
        {
            throw WorkerXvmError("xvm.typed_input_invalid",
                "xvm_program requires canonical typed input SHA-256 and mix64 values");
        }
        std::array<uint32_t, WorkerXvmInputWordCountValue> words{};
        words[0] = HexWord(boundInputMix64, 0);
        words[1] = HexWord(boundInputMix64, 8);
        for (size_t index = 0; index < 8; ++index)
        {
            words[index + 2] = HexWord(boundInputSha256, index * 8);
        }
        words[10] = inputControlPass ? 1u : 0u;
        return words;
    }

    WorkerXvmCpuTieredCheckpointResult WorkerRunXvmCpuTieredCheckpoint(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        bool cpuTieredBackend,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested)
    {
        WorkerXvmCpuTieredCheckpointResult result;
        result.state = initialState;
        auto started = std::chrono::steady_clock::now();
        if (!cpuTieredBackend)
        {
            result.status = WorkerAdvanceXvmCpuReference(
                program, limits, inputs, result.state, checkpointFuel, cancelRequested);
            result.elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return result;
        }
        result.status = WorkerAdvanceXvmCpuPlan(
            program, plan, limits, inputs, result.state, checkpointFuel, result.telemetry, cancelRequested);
        result.elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (result.status == WorkerXvmAdvanceStatus::Canceled)
        {
            return result;
        }

        auto referenceState = initialState;
        auto referenceStatus = WorkerAdvanceXvmCpuReference(
            program, limits, inputs, referenceState, checkpointFuel, {});
        if (referenceStatus != result.status ||
            !WorkerXvmMachineStatesEqual(referenceState, result.state))
        {
            throw WorkerXvmError("xvm.cpu_plan_checkpoint_mismatch",
                "CPU plan/codelet checkpoint state differs from the canonical interpreter");
        }
        return result;
    }

    WorkerXvmCpuTieredDifferentialResult WorkerRunXvmCpuTieredDifferential(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        std::function<bool()> const& cancelRequested,
        WorkerXvmRunResult const* admittedReferenceResult,
        double admittedReferenceElapsedMs)
    {
        WorkerXvmRunResult referenceResult;
        double referenceElapsedMs = admittedReferenceElapsedMs;
        if (admittedReferenceResult)
        {
            referenceResult = *admittedReferenceResult;
        }
        else
        {
            auto referenceState = initialState;
            auto started = std::chrono::steady_clock::now();
            auto status = WorkerAdvanceXvmCpuReference(
                program, limits, inputs, referenceState, 0, cancelRequested);
            referenceElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (status == WorkerXvmAdvanceStatus::Canceled)
            {
                return Canceled(std::move(referenceState), referenceElapsedMs);
            }
            referenceResult = WorkerXvmMachineResult(referenceState);
        }

        WorkerXvmCpuPlanTelemetry coldTelemetry;
        auto coldState = initialState;
        auto coldStarted = std::chrono::steady_clock::now();
        auto coldStatus = WorkerAdvanceXvmCpuPlan(
            program, plan, limits, inputs, coldState, 0, coldTelemetry, cancelRequested);
        auto coldElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - coldStarted).count();
        if (coldStatus == WorkerXvmAdvanceStatus::Canceled)
        {
            return Canceled(std::move(coldState), referenceElapsedMs);
        }

        WorkerXvmCpuPlanTelemetry warmTelemetry;
        auto warmState = initialState;
        auto warmStarted = std::chrono::steady_clock::now();
        auto warmStatus = WorkerAdvanceXvmCpuPlan(
            program, plan, limits, inputs, warmState, 0, warmTelemetry, cancelRequested);
        auto warmElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - warmStarted).count();
        if (warmStatus == WorkerXvmAdvanceStatus::Canceled)
        {
            return Canceled(std::move(warmState), referenceElapsedMs);
        }

        auto coldResult = WorkerXvmMachineResult(coldState);
        auto warmResult = WorkerXvmMachineResult(warmState);
        if (!WorkerXvmRunResultsEqual(referenceResult, coldResult) ||
            !WorkerXvmRunResultsEqual(referenceResult, warmResult))
        {
            throw WorkerXvmError("xvm.cpu_plan_differential_mismatch",
                "CPU plan/codelet result differs from the canonical interpreter; candidate quarantined before publication");
        }

        WorkerXvmCpuTieredDifferentialResult result;
        result.result = std::move(warmResult);
        result.telemetry = warmTelemetry;
        result.referenceElapsedMs = referenceElapsedMs;
        result.coldElapsedMs = coldElapsedMs;
        result.warmElapsedMs = warmElapsedMs;
        return result;
    }

    std::wstring WorkerXvmCpuTieredExecutionJson(
        WorkerXvmCpuExecutionPlan const& plan,
        double fullGraphPlanBuildMs,
        double executionRebuildMs,
        WorkerXvmCpuTieredDifferentialResult const& result,
        bool checkpointVerified,
        double checkpointElapsedMs,
        bool deoptimizedToInterpreter)
    {
        auto warmSpeedup = result.warmElapsedMs > 0.0
            ? result.referenceElapsedMs / result.warmElapsedMs
            : 0.0;
        auto json = std::wstring(L"{\"enabled\":true") +
            L",\"schema_version\":\"xvm-cpu-tiered-execution-result-v1\"" +
            L",\"gate_id\":\"XVM_CPU_PLAN_CODELET_DIFFERENTIAL_V1\"" +
            L",\"plan\":" + WorkerXvmCpuExecutionPlanJson(plan) +
            L",\"full_graph_plan_build_ms\":" + DoubleJson(fullGraphPlanBuildMs) +
            L",\"execution_rebuild_ms\":" + DoubleJson(executionRebuildMs) +
            L",\"reference_elapsed_ms\":" + DoubleJson(result.referenceElapsedMs) +
            L",\"cold_elapsed_ms\":" + DoubleJson(result.coldElapsedMs) +
            L",\"warm_elapsed_ms\":" + DoubleJson(result.warmElapsedMs) +
            L",\"warm_speedup_ratio\":" + DoubleJson(warmSpeedup) +
            L",\"source_instructions_executed\":" + std::to_wstring(result.telemetry.sourceInstructionsExecuted) +
            L",\"dispatch_count\":" + std::to_wstring(result.telemetry.dispatchCount) +
            L",\"instruction_dispatch_count\":" + std::to_wstring(result.telemetry.instructionDispatchCount) +
            L",\"codelet_dispatch_count\":" + std::to_wstring(result.telemetry.codeletDispatchCount) +
            L",\"dispatch_reduction\":" + std::to_wstring(
                result.telemetry.sourceInstructionsExecuted - result.telemetry.dispatchCount) +
            L",\"cold_exact_match\":true,\"warm_exact_match\":true" +
            L",\"fuel_exact_match\":true,\"trap_exact_match\":true";
        if (checkpointVerified)
        {
            json += L",\"checkpoint_elapsed_ms\":" + DoubleJson(checkpointElapsedMs) +
                L",\"checkpoint_exact_match\":true" +
                L",\"snapshot_schema\":\"xvm-state-snapshot-v4\"" +
                L",\"snapshot_plan_bound\":true,\"snapshot_backend_bound\":true" +
                L",\"deoptimized_to_interpreter\":" +
                    (deoptimizedToInterpreter ? L"true" : L"false") +
                L",\"deoptimization_exact_match\":true,\"arbitrary_pc_migration\":false";
        }
        return json +
            L",\"canonical_backend\":\"cpu_reference\"" +
            L",\"selected_backend\":\"cpu_plan_codelet\"" +
            L",\"fallback_backend\":\"cpu_reference\"" +
            L",\"fallback_available\":true,\"candidate_quarantined\":false" +
            L",\"mismatch_policy\":\"quarantine_before_publication\"" +
            L",\"runtime_native_codegen\":false,\"executable_upload\":false}";
    }
}
