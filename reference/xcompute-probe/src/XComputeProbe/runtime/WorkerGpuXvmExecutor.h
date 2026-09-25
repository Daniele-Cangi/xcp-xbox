#pragma once

#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmTypes.h"

#include <array>
#include <functional>
#include <string>

namespace XComputeProbe
{
    struct WorkerGpuXvmExecutionResult
    {
        WorkerXvmRunResult run;
        std::wstring telemetryJson;
    };

    WorkerGpuXvmExecutionResult WorkerExecuteGpuXvmProfile(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerGpuXvmExecutionPlan const& executionPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested);
}
