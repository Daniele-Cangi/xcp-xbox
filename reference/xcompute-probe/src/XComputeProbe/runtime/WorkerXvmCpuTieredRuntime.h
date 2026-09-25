#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "WorkerXvmCpuExecutionPlan.h"

namespace XComputeProbe
{
    struct WorkerXvmCpuTieredCheckpointResult
    {
        WorkerXvmMachineState state;
        WorkerXvmAdvanceStatus status = WorkerXvmAdvanceStatus::Halted;
        WorkerXvmCpuPlanTelemetry telemetry;
        double elapsedMs = 0.0;
    };

    struct WorkerXvmCpuTieredDifferentialResult
    {
        WorkerXvmRunResult result;
        WorkerXvmMachineState canceledState;
        WorkerXvmCpuPlanTelemetry telemetry;
        double referenceElapsedMs = 0.0;
        double coldElapsedMs = 0.0;
        double warmElapsedMs = 0.0;
        bool canceled = false;
    };

    std::array<uint32_t, WorkerXvmInputWordCountValue> WorkerBuildXvmInputWords(
        std::wstring const& boundInputSha256,
        std::wstring const& boundInputMix64,
        bool inputControlPass);

    WorkerXvmCpuTieredCheckpointResult WorkerRunXvmCpuTieredCheckpoint(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        bool cpuTieredBackend,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested);

    WorkerXvmCpuTieredDifferentialResult WorkerRunXvmCpuTieredDifferential(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        std::function<bool()> const& cancelRequested,
        WorkerXvmRunResult const* admittedReferenceResult = nullptr,
        double admittedReferenceElapsedMs = 0.0);

    std::wstring WorkerXvmCpuTieredExecutionJson(
        WorkerXvmCpuExecutionPlan const& plan,
        double fullGraphPlanBuildMs,
        double executionRebuildMs,
        WorkerXvmCpuTieredDifferentialResult const& result,
        bool checkpointVerified,
        double checkpointElapsedMs,
        bool deoptimizedToInterpreter);
}
