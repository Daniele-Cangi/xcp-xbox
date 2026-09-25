#pragma once

#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerGpuXvmProfileContract.h"
#include "WorkerXvmIsa.h"

#include <array>
#include <functional>

namespace XComputeProbe
{
    struct WorkerGpuXvmExecutionResult;

    WorkerGpuXvmExecutionResult WorkerExecuteGpuXvmProvableWorlds(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGpuXvmExecutionPlan const& executionPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested);
}
