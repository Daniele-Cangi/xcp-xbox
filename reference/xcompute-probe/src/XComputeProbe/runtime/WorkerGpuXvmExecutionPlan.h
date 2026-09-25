#pragma once

#include "WorkerGpuXvmProfileContract.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersion =
        L"gpu-xvm-execution-plan-v2";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV3 =
        L"gpu-xvm-execution-plan-v3";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV4 =
        L"gpu-xvm-execution-plan-v4";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV5 =
        L"gpu-xvm-execution-plan-v5";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV6 =
        L"gpu-xvm-execution-plan-v6";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV7 =
        L"gpu-xvm-execution-plan-v7";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV8 =
        L"gpu-xvm-execution-plan-v8";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV9 =
        L"gpu-xvm-execution-plan-v9";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV10 =
        L"gpu-xvm-execution-plan-v10";
    inline constexpr wchar_t const* WorkerGpuXvmExecutionPlanSchemaVersionV11 =
        L"gpu-xvm-execution-plan-v11";
    inline constexpr wchar_t const* WorkerGpuXvmTrapStateSchemaVersion =
        L"xvm-trap-state-v1";
    inline constexpr uint32_t WorkerGpuXvmCallFrameCapacityValue = 8;
    inline constexpr uint32_t WorkerGpuXvmReturnPcStackWordOffsetValue = 60;
    inline constexpr uint32_t WorkerGpuXvmCallLoopDepthStackWordOffsetValue = 68;
    inline constexpr uint32_t WorkerGpuXvmTrapStatusWordOffsetValue = 78;
    inline constexpr uint32_t WorkerGpuXvmTrapCodeWordOffsetValue = 79;
    inline constexpr uint32_t WorkerGpuXvmTrapPcWordOffsetValue = 80;
    inline constexpr uint32_t WorkerGpuXvmTrapRecoveryPcWordOffsetValue = 81;
    inline constexpr uint32_t WorkerGpuXvmTrapOccurrenceCountWordOffsetValue = 82;
    inline constexpr uint32_t WorkerGpuXvmTrapStatusNoneValue = 0;
    inline constexpr uint32_t WorkerGpuXvmTrapStatusRecoveredValue = 1;
    inline constexpr uint32_t WorkerGpuXvmTrapCodeMaximumValue = 65535;

    struct WorkerGpuXvmVerifiedCodeRegion
    {
        uint32_t regionIndex = 0;
        uint32_t beginPc = 0;
        uint32_t endPc = 0;
        bool mainRegion = false;
    };

    struct WorkerGpuXvmVerifiedCallSite
    {
        uint32_t callPc = 0;
        uint32_t callerRegionIndex = 0;
        uint32_t targetEntryPc = 0;
        uint32_t targetRegionIndex = 0;
    };

    struct WorkerGpuXvmVerifiedRecoverySite
    {
        uint32_t trapPc = 0;
        uint32_t trapCode = 0;
        uint32_t recoveryPc = 0;
        uint32_t regionIndex = 0;
    };

    struct WorkerGpuXvmVerifiedBranchSite
    {
        uint32_t branchPc = 0;
        uint32_t registerIndex = 0;
        uint32_t targetPc = 0;
        uint32_t regionIndex = 0;
    };

    struct WorkerGpuXvmExecutionPlan
    {
        bool admitted = false;
        std::wstring schemaVersion;
        std::wstring profileId;
        std::wstring contractId;
        std::wstring contractSha256;
        std::wstring shaderSha256;
        std::wstring programId;
        std::wstring programSha256;
        std::wstring planSha256;
        uint32_t instructionCount = 0;
        uint32_t staticWorstCaseFuel = 0;
        uint32_t epochInstructionBudget = 0;
        uint32_t requiredEpochCount = 0;
        uint32_t maxEpochs = 0;
        uint32_t capabilityFlags = 0;
        uint32_t laneCount = 0;
        std::array<uint32_t, 3> gridShape{};
        std::array<uint32_t, 3> workgroupShape{};
        std::array<uint32_t, 3> dispatchGroupShape{};
        uint32_t workgroupThreadCount = 0;
        uint32_t stateWordsPerLane = 0;
        uint32_t totalStateWords = 0;
        uint32_t inputWordsPerLane = 0;
        bool manyLaneAdmitted = false;
        bool spmdLaneContextSeed = false;
        bool generalizedTopologyAdmitted = false;
        std::wstring spmdExecutionPlanSha256;
        uint32_t maxCallDepth = 0;
        std::wstring stateSchemaVersion;
        uint32_t maxLoopDepth = 0;
        uint32_t loopFrameCapacity = 0;
        uint32_t loopBeginPcWordOffset = 0;
        uint32_t loopEndPcWordOffset = 0;
        uint32_t loopRemainingWordOffset = 0;
        uint32_t callFrameCapacity = 0;
        uint32_t returnPcStackWordOffset = 0;
        uint32_t callLoopDepthStackWordOffset = 0;
        uint32_t verifiedActualMaxCallDepth = 0;
        bool callGraphAllFunctionsReachable = false;
        bool callGraphAcyclic = false;
        std::wstring trapStateSchemaVersion;
        uint32_t trapStatusWordOffset = 0;
        uint32_t trapCodeWordOffset = 0;
        uint32_t trapPcWordOffset = 0;
        uint32_t trapRecoveryPcWordOffset = 0;
        uint32_t trapOccurrenceCountWordOffset = 0;
        uint32_t trapCodeMaximum = 0;
        bool recoveredTrapStateAdmitted = false;
        bool recoverySameOwnershipVerified = false;
        std::vector<WorkerGpuXvmVerifiedCodeRegion> verifiedCodeRegions;
        bool legacyBranchTopologyAdmitted = false;
        bool branchTargetsForwardSameRegionVerified = false;
        bool structuredControlMixingForbidden = false;
        std::vector<WorkerGpuXvmVerifiedCallSite> verifiedCallSites;
        std::vector<WorkerGpuXvmVerifiedRecoverySite> verifiedRecoverySites;
        std::vector<WorkerGpuXvmVerifiedBranchSite> verifiedBranchSites;
        bool microtraceAdmitted = false;
        std::wstring microtraceSchemaVersion;
        std::wstring microtraceSha256;
        uint32_t microtraceWordsPerOp = 0;
        uint32_t microtraceOpCount = 0;
        uint32_t microtraceFuelDebit = 0;
        uint32_t microtraceExpansionNumerator = 0;
        uint32_t microtraceExpansionDenominator = 0;
        uint32_t microtraceBasicBlockCount = 0;
        uint32_t microtraceSuperblockCount = 0;
        uint32_t microtraceFusedRecordCount = 0;
        uint32_t microtraceConstantFoldRecordCount = 0;
        uint32_t microtraceAluFusionRecordCount = 0;
        std::vector<uint32_t> microtraceSourcePcMap;
        std::vector<uint32_t> microtraceRecordSourcePcMap;
        std::vector<uint32_t> microtraceRecordSourceSpanMap;
        std::vector<uint32_t> microtraceRecordKindMap;
        std::vector<uint32_t> microtraceSuperblockSourcePcMap;
        std::vector<uint32_t> microtraceSuperblockSourceSpanMap;
        std::vector<uint32_t> microtraceWords;
    };

    WorkerGpuXvmExecutionPlan WorkerBuildGpuXvmExecutionPlan(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        std::wstring const& programSha256,
        std::wstring const& spmdExecutionPlanSha256,
        uint32_t spmdLaneCount,
        std::array<uint32_t, 3> const& spmdGridShape,
        std::array<uint32_t, 3> const& spmdWorkgroupShape);

    std::wstring WorkerGpuXvmExecutionPlanJson(WorkerGpuXvmExecutionPlan const& plan);
    bool WorkerGpuXvmExecutionPlansEqual(
        WorkerGpuXvmExecutionPlan const& expected,
        WorkerGpuXvmExecutionPlan const& actual);
}
