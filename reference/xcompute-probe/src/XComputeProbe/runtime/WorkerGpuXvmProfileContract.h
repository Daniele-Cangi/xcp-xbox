#pragma once

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmTypes.h"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace XComputeProbe
{
    inline constexpr uint64_t WorkerGpuXvmMaxInstructionsValue = 15;
    inline constexpr uint64_t WorkerGpuXvmMemoryBytesValue = 64;
    inline constexpr uint64_t WorkerGpuXvmOutputBytesValue = 16;
    inline constexpr uint64_t WorkerGpuXvmFuelLimitValue = 256;
    inline constexpr uint32_t WorkerGpuXvmParameterWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmInputWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmResultWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmResultOutputWordOffsetValue = 8;
    inline constexpr uint64_t WorkerGpuXvmGeneralMaxInstructionsValue = 32;
    inline constexpr uint64_t WorkerGpuXvmGeneralMemoryBytesValue = 64;
    inline constexpr uint64_t WorkerGpuXvmGeneralOutputBytesValue = 16;
    inline constexpr uint64_t WorkerGpuXvmGeneralFuelLimitValue = 64;
    inline constexpr uint32_t WorkerGpuXvmGeneralParameterWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmGeneralInputWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmGeneralProgramWordsValue = 128;
    inline constexpr uint32_t WorkerGpuXvmGeneralResultWordsValue = 48;
    inline constexpr uint32_t WorkerGpuXvmGeneralResultOutputWordOffsetValue = 8;
    inline constexpr uint32_t WorkerGpuXvmResidentEpochInstructionBudgetValue = 8;
    inline constexpr uint32_t WorkerGpuXvmResidentMaxEpochsValue = 4;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2ParameterWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2InputWordsValue = 16;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2ProgramWordsValue = 128;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2StateWordsPerLaneValue = 96;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2LoopControlStateWordsPerLaneValue = 128;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2FullCallReturnStateWordsPerLaneValue = 128;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2RecoveredTrapStateWordsPerLaneValue = 128;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2LegacyBranchStateWordsPerLaneValue = 128;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2EpochInstructionBudgetValue = 8;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2MaxEpochsValue = 8;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2LoopControlMaxDepthValue = 8;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2FullCallReturnMaxDepthValue = 8;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2RecoveredTrapMaxDepthValue = 8;
    inline constexpr uint32_t WorkerGpuXvmFullIsaV2LegacyBranchMaxDepthValue = 8;
    inline constexpr uint32_t WorkerGpuXvmCapabilityTypedMemoryValue = 1u << 0;
    inline constexpr uint32_t WorkerGpuXvmCapabilityStructuredControlValue = 1u << 1;
    inline constexpr uint32_t WorkerGpuXvmCapabilityStructuredControlPhaseBValue = 1u << 2;
    inline constexpr uint32_t WorkerGpuXvmCapabilitySpmdLaneContextValue = 1u << 3;

    enum class WorkerGpuXvmProfileKind
    {
        M150SeedStraightLineV1,
        GeneralStraightLineU32V1,
        ResidentCoreStraightLineU32V1,
        FullIsaV2TypedStructuredU32V1,
        FullIsaV2LoopControlU32V1,
        FullIsaV2FullCallReturnU32V1,
        FullIsaV2RecoveredTrapU32V1,
        FullIsaV2LegacyBranchU32V1,
        SpmdManyLanePhaseB4x3U32V1,
        SpmdManyLaneGeneralized8x8U32V1,
        ProvableWorlds1024x1024U32V1,
        MicrotraceIdentityStraightLineU32V1,
        MicrotraceOptimizedStraightLineU32V1,
    };

    struct WorkerGpuXvmProfileDescriptor
    {
        WorkerGpuXvmProfileKind kind;
        wchar_t const* schemaVersion;
        wchar_t const* contractId;
        wchar_t const* contractSha256;
        wchar_t const* profileId;
        wchar_t const* shaderName;
        wchar_t const* shaderSha256;
        wchar_t const* shaderSourceSha256;
        wchar_t const* computeKind;
        uint64_t maxInstructions;
        uint64_t memoryBytes;
        uint64_t outputBytes;
        uint64_t fuelLimit;
        uint32_t parameterWords;
        uint32_t inputWords;
        uint32_t programWords;
        uint32_t resultWords;
        uint32_t resultOutputWordOffset;
        bool requiresProgramSrv;
        bool sequentialDispatchPerInstruction;
        bool residentExecution;
        uint32_t epochInstructionBudget;
        uint32_t maxEpochs;
        bool liveMeasured;
        bool laneParametric;
        bool staticFuelDispatchBound;
        bool pcMustAdvanceMonotonically;
        bool terminalPcIsInstructionCount;
        uint32_t laneCount;
        std::array<uint32_t, 3> gridShape;
        std::array<uint32_t, 3> workgroupShape;
        uint32_t stateWordsPerLane;
        uint32_t inputWordsPerLane;
        uint32_t capabilityFlags;
        uint32_t maxCallDepth;
        wchar_t const* executionPlanSchemaVersion = nullptr;
        wchar_t const* stateSchemaVersion = nullptr;
        uint32_t maxLoopDepth = 0;
        bool manyLaneAdmitted = false;
        bool spmdLaneContextSeed = false;
        bool microtraceAdmitted = false;
        uint32_t microtraceWordsPerOp = 0;
        uint32_t maxMicrotraceOps = 0;
        bool generalizedTopologyAdmitted = false;
        uint32_t maxWorkgroupThreads = 1;
    };


    wchar_t const* WorkerGpuXvmProfileContractSchemaVersion();
    wchar_t const* WorkerGpuXvmProfileContractId();
    wchar_t const* WorkerGpuXvmProfileContractSha256();
    wchar_t const* WorkerGpuXvmProfileId();
    wchar_t const* WorkerGpuXvmShaderName();
    wchar_t const* WorkerGpuXvmShaderSha256();
    wchar_t const* WorkerGpuXvmShaderSourceSha256();
    wchar_t const* WorkerGpuXvmInstructionWordsSha256();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmM150Profile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmGeneralV1Profile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmResidentCoreV1Profile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotraceV1Profile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotracePhaseBV1Profile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2TypedStructuredProfile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2LoopControlProfile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2FullCallReturnProfile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2RecoveredTrapProfile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2LegacyBranchProfile();
    WorkerGpuXvmProfileDescriptor const* WorkerFindGpuXvmProfile(std::wstring const& profileId);


    bool WorkerGpuXvmProfileMatches(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);
    bool WorkerGpuXvmProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);

    std::wstring WorkerGpuXvmHashBytes(std::vector<uint8_t> const& bytes);
    bool WorkerGpuXvmShaderMatchesContract(
        std::vector<uint8_t> const& bytes,
        std::wstring& actualSha256);
    bool WorkerGpuXvmShaderMatchesContract(
        WorkerGpuXvmProfileDescriptor const& profile,
        std::vector<uint8_t> const& bytes,
        std::wstring& actualSha256);

    bool WorkerGpuXvmIsDeviceLoss(HRESULT hr);
    std::wstring WorkerGpuXvmProfileContractsJson();
}
