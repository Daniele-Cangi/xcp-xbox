#pragma once

#include <array>
#include <functional>
#include <string>

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    enum class WorkerXvmAdvanceStatus
    {
        Halted,
        Checkpoint,
        Canceled,
    };

    WorkerXvmMachineState WorkerCreateXvmMachineState(
        WorkerGraphNodeResourceLimits const& limits);

    void WorkerValidateXvmMachineState(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerXvmMachineState const& state);

    WorkerXvmAdvanceStatus WorkerAdvanceXvmCpuReference(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState& state,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested = {});

    WorkerXvmRunResult WorkerXvmMachineResult(WorkerXvmMachineState const& state);

    WorkerXvmRunResult WorkerRunXvmCpuReference(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs);

    std::wstring WorkerXvmBytesHex(std::vector<uint8_t> const& bytes);
}
