#pragma once

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerXvmSpmdExecutionPlanSchemaVersion =
        L"xvm-spmd-execution-plan-v1";
    inline constexpr wchar_t const* WorkerXvmSpmdContractId =
        L"GPU_XVM_SPMD_MANY_LANE_V1_PHASE_A_CPU_CANONICAL";
    inline constexpr wchar_t const* WorkerXvmSpmdExecutionPlanSchemaVersionV2 =
        L"xvm-spmd-execution-plan-v2";
    inline constexpr wchar_t const* WorkerXvmSpmdGeneralizedCpuContractId =
        L"GPU_XVM_SPMD_MANY_LANE_V1_GENERALIZED_CPU_CANONICAL";
    inline constexpr wchar_t const* WorkerXvmSpmdExecutionPlanSchemaVersionV3 =
        L"xvm-spmd-execution-plan-v3";
    inline constexpr wchar_t const* WorkerXvmProvableWorldsCpuContractId =
        L"PROVABLE_WORLDS_V1_CPU_CANONICAL";
    inline constexpr wchar_t const* WorkerXvmSpmdLaneContextSchemaVersion =
        L"xvm-spmd-lane-context-v1";
    inline constexpr uint32_t WorkerXvmSpmdMaxLogicalLanesValue = 4096;
    inline constexpr uint32_t WorkerXvmSpmdLaneIdRegisterValue = 12;
    inline constexpr uint32_t WorkerXvmSpmdLaneXRegisterValue = 13;
    inline constexpr uint32_t WorkerXvmSpmdLaneYRegisterValue = 14;
    inline constexpr uint32_t WorkerXvmSpmdLaneZRegisterValue = 15;

    struct WorkerXvmSpmdExecutionPlan
    {
        bool admitted = false;
        std::wstring schemaVersion;
        std::wstring contractId;
        std::wstring programId;
        std::wstring programSha256;
        std::wstring planSha256;
        std::array<uint32_t, 3> gridShape{};
        std::array<uint32_t, 3> workgroupShape{};
        uint32_t laneCount = 0;
        uint64_t perLaneWorstCaseFuel = 0;
        uint64_t aggregateWorstCaseFuel = 0;
        uint64_t perLaneMemoryBytes = 0;
        uint64_t totalPrivateMemoryBytes = 0;
        uint64_t perLaneOutputBytes = 0;
        uint64_t totalOutputBytes = 0;
    };

    struct WorkerXvmSpmdRunResult
    {
        WorkerXvmRunResult aggregate;
        uint32_t laneCount = 0;
        uint32_t firstFailControlLane = UINT32_MAX;
        uint32_t aggregateTrapLane = UINT32_MAX;
    };

    WorkerXvmSpmdExecutionPlan WorkerBuildXvmSpmdExecutionPlan(
        std::array<uint32_t, 3> const& gridShape,
        std::array<uint32_t, 3> const& workgroupShape,
        WorkerXvmProgram const& program,
        WorkerXvmStaticFuelProof const& staticFuelProof,
        WorkerGraphNodeResourceLimits const& limits,
        std::wstring const& programSha256);

    std::wstring WorkerXvmSpmdExecutionPlanJson(WorkerXvmSpmdExecutionPlan const& plan);
    bool WorkerXvmSpmdExecutionPlansEqual(
        WorkerXvmSpmdExecutionPlan const& expected,
        WorkerXvmSpmdExecutionPlan const& actual);

    WorkerXvmSpmdRunResult WorkerRunXvmSpmdCpuReference(
        WorkerXvmSpmdExecutionPlan const& plan,
        WorkerXvmProgram const& program,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& sharedInputs,
        std::function<bool()> const& cancelRequested = {});
}
