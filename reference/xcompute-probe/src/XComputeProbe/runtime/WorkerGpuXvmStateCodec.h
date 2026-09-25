#pragma once

#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmExecutionPlan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace XComputeProbe
{
    inline constexpr uint32_t WorkerGpuXvmResidentPcWordOffsetValue = 12;
    inline constexpr uint32_t WorkerGpuXvmResidentEpochWordOffsetValue = 13;
    inline constexpr uint32_t WorkerGpuXvmLaneCallDepthWordOffsetValue = 58;
    inline constexpr uint32_t WorkerGpuXvmLaneReturnPcWordOffsetValue =
        WorkerGpuXvmReturnPcStackWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneCallLoopDepthWordOffsetValue =
        WorkerGpuXvmCallLoopDepthStackWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneCallFrameCapacityValue =
        WorkerGpuXvmCallFrameCapacityValue;
    inline constexpr uint32_t WorkerGpuXvmLaneLoopDepthWordOffsetValue = 76;
    inline constexpr uint32_t WorkerGpuXvmLaneMaxLoopDepthWordOffsetValue = 77;
    // Legacy lane-state v2/v3 reserved word. State v4 assigns word 78 to trap status.
    inline constexpr uint32_t WorkerGpuXvmLaneTrapCodeWordOffsetValue = 78;
    inline constexpr uint32_t WorkerGpuXvmLaneTrapStatusWordOffsetV4Value =
        WorkerGpuXvmTrapStatusWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneTrapCodeWordOffsetV4Value =
        WorkerGpuXvmTrapCodeWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneTrapPcWordOffsetV4Value =
        WorkerGpuXvmTrapPcWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneTrapRecoveryPcWordOffsetV4Value =
        WorkerGpuXvmTrapRecoveryPcWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLaneTrapOccurrenceCountWordOffsetV4Value =
        WorkerGpuXvmTrapOccurrenceCountWordOffsetValue;
    inline constexpr uint32_t WorkerGpuXvmLanePlanVersionWordOffsetValue = 83;
    inline constexpr uint32_t WorkerGpuXvmLaneLoopBeginPcWordOffsetValue = 86;
    inline constexpr uint32_t WorkerGpuXvmLaneLoopEndPcWordOffsetValue = 94;
    inline constexpr uint32_t WorkerGpuXvmLaneLoopRemainingWordOffsetValue = 102;
    inline constexpr uint32_t WorkerGpuXvmLaneLoopFrameCapacityValue = 8;

    struct WorkerGpuXvmDecodedState
    {
        bool halted = false;
        uint32_t control = 0;
        uint32_t fuelConsumed = 0;
        uint32_t trapCode = 0;
        WorkerXvmTrapState trap;
        uint32_t faultCode = 0;
        uint32_t pc = 0;
        uint32_t epochSequence = 0;
        uint32_t callDepth = 0;
        uint32_t loopDepth = 0;
        std::vector<uint32_t> outputWords;
    };

    std::vector<uint32_t> WorkerCreateGpuXvmInitialState(
        WorkerGpuXvmProfileDescriptor const& profile,
        uint32_t instructionCount,
        uint32_t memoryWords,
        uint32_t outputWords,
        uint32_t fuelLimit,
        WorkerGpuXvmExecutionPlan const& plan);

    bool WorkerDecodeGpuXvmState(
        WorkerGpuXvmProfileDescriptor const& profile,
        std::vector<uint32_t> const& words,
        uint32_t instructionCount,
        uint32_t memoryWords,
        uint32_t outputWords,
        uint32_t fuelLimit,
        WorkerGpuXvmExecutionPlan const& plan,
        WorkerXvmProgram const& program,
        WorkerGpuXvmDecodedState& decoded,
        std::string& error,
        uint32_t laneId = 0);
}
