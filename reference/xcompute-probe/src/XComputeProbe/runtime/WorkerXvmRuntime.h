#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphPublishing.h"
#include "WorkerXvmAdmission.h"
#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerXvmSnapshotAuthority.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    struct WorkerXvmExecutionInput
    {
        winrt::Windows::Data::Json::JsonObject node{ nullptr };
        std::wstring protocolVersion;
        std::wstring executionId;
        std::wstring nodeId;
        std::wstring resultArtifactId;
        std::wstring boundInputSha256;
        std::wstring boundInputMix64;
        std::wstring inputEdgesJson;
        WorkerXvmSnapshotAuthorityProvider snapshotAuthorityProvider;
        std::function<bool()> cancelRequested;
        bool inputControlPass = true;
        uint64_t inputEdgeCount = 0;
        WorkerGpuXvmExecutionPlan admittedGpuExecutionPlan;
        bool hasAdmittedGpuExecutionPlan = false;
        WorkerXvmSpmdExecutionPlan admittedSpmdExecutionPlan;
        bool hasAdmittedSpmdExecutionPlan = false;
        WorkerXvmCpuExecutionPlan admittedCpuExecutionPlan;
        bool hasAdmittedCpuExecutionPlan = false;
        WorkerXvmCpuCapsulePlan admittedCpuCapsulePlan;
        bool hasAdmittedCpuCapsulePlan = false;
        WorkerCpuGpuConvergencePlan admittedBackendConvergencePlan;
        bool hasAdmittedBackendConvergencePlan = false;
        WorkerVerifiedProductionModePlan admittedProductionModePlan;
        bool hasAdmittedProductionModePlan = false;
        double admittedCpuPlanBuildElapsedMs = 0.0;
    };

    struct WorkerXvmPreAdmissionResult
    {
        std::wstring staticFuelProofJson = L"null";
        std::wstring gpuExecutionPlanJson = L"null";
        WorkerGpuXvmExecutionPlan gpuExecutionPlan;
        std::wstring spmdExecutionPlanJson = L"null";
        WorkerXvmSpmdExecutionPlan spmdExecutionPlan;
        std::wstring cpuExecutionPlanJson = L"null";
        WorkerXvmCpuExecutionPlan cpuExecutionPlan;
        std::wstring cpuCapsulePlanJson = L"null";
        WorkerXvmCpuCapsulePlan cpuCapsulePlan;
        std::wstring backendConvergencePlanJson = L"null";
        WorkerCpuGpuConvergencePlan backendConvergencePlan;
        std::wstring productionModePlanJson = L"null";
        WorkerVerifiedProductionModePlan productionModePlan;
        double cpuPlanBuildElapsedMs = 0.0;
    };

    struct WorkerXvmExecutionResult
    {
        WorkerGraphPublishedArtifact published;
        std::wstring programId;
        std::wstring programSha256;
        std::wstring logicalSha256;
        std::wstring computeMix64;
        std::wstring controlToken;
        std::wstring outputHex;
        std::wstring backend;
        std::wstring computeKind;
        std::wstring functionalCanonicalJson;
        std::wstring gpuDifferentialJson;
        std::wstring microtraceExecutionJson;
        std::wstring spmdExecutionJson;
        std::wstring provableWorldsJson;
        std::wstring backendConvergenceJson;
        std::wstring productionModeJson;
        std::wstring cpuTieredExecutionJson;
        std::wstring typedMemoryJson;
        std::wstring structuredControlJson;
        std::wstring staticFuelProofJson;
        std::wstring stateSnapshotJson;
        std::wstring stateResumeJson;
        std::wstring resourceUsageJson;
        uint64_t programBytes = 0;
        uint64_t instructionCount = 0;
        uint64_t staticWorstCaseFuel = 0;
        uint64_t fuelConsumed = 0;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
        bool gpuDifferential = false;
        bool spmdExecution = false;
        bool ok = false;
        bool verified = false;
    };

    using WorkerXvmSha256Text = std::function<std::wstring(std::string const& text)>;
    using WorkerXvmWideMix64 = std::function<uint64_t(std::wstring const& text, uint64_t seed)>;

    wchar_t const* WorkerXvmIsaVersion();
    wchar_t const* WorkerXvmLatestIsaVersion();
    wchar_t const* WorkerXvmProgramArtifactSchemaVersion();
    std::wstring WorkerXvmIsaCatalogJson();
    uint64_t WorkerXvmMaxProgramBytes();
    uint64_t WorkerXvmMaxInstructions();
    uint64_t WorkerXvmMaxLoopIterations();
    uint64_t WorkerXvmMaxLoopDepth();
    uint64_t WorkerXvmMaxCallDepth();

    std::wstring WorkerValidateXvmProgramAdmission(
        winrt::Windows::Data::Json::JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerXvmSnapshotAuthorityProvider const& snapshotAuthorityLoader);

    WorkerXvmPreAdmissionResult WorkerPreAdmitXvmProgram(
        winrt::Windows::Data::Json::JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerXvmSnapshotAuthorityProvider const& snapshotAuthorityLoader);

    WorkerXvmExecutionResult WorkerExecuteXvmProgram(
        WorkerXvmExecutionInput const& input,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerGraphArtifactPublisher const& publisher,
        WorkerXvmSha256Text const& sha256Text,
        WorkerXvmWideMix64 const& wideMix64);
}
