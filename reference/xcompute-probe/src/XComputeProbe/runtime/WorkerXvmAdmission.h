#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphResourceLedger.h"
#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerGpuXvmProfileContract.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmCpuExecutionPlan.h"
#include "WorkerXvmCpuCapsuleBackend.h"
#include "WorkerCpuGpuConvergenceBackend.h"
#include "WorkerVerifiedProductionMode.h"
#include "WorkerXvmSnapshotAuthority.h"
#include "WorkerXvmSpmdRuntime.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    inline constexpr uint64_t WorkerXvmProgramArtifactMaxBytesValue = 256ull * 1024ull;

    using WorkerXvmSnapshotAuthorityProvider = std::function<WorkerXvmSnapshotAuthority()>;

    struct WorkerXvmProgramReadTarget
    {
        std::filesystem::path path;
        std::wstring artifactKind;
        std::wstring sha256;
        uint64_t bytes = 0;
        bool committed = false;
    };

    struct WorkerXvmAdmission
    {
        WorkerXvmProgram program;
        WorkerXvmStaticFuelProof staticFuelProof;
        WorkerXvmCpuExecutionPlan cpuExecutionPlan;
        WorkerXvmCpuCapsulePlan cpuCapsulePlan;
        WorkerCpuGpuConvergencePlan backendConvergencePlan;
        WorkerVerifiedProductionModePlan productionModePlan;
        double cpuPlanBuildElapsedMs = 0.0;
        WorkerXvmProgramReadTarget target;
        WorkerGraphNodeResourceLimits limits;
        std::wstring expectedProgramSha256;
        std::wstring backend;
        WorkerGpuXvmProfileDescriptor const* gpuProfile = nullptr;
        WorkerGpuXvmExecutionPlan gpuExecutionPlan;
        WorkerXvmSpmdExecutionPlan spmdExecutionPlan;
        std::wstring provableWorldsFieldArtifactId;
        std::wstring provableWorldsVerificationArtifactId;
        std::wstring provableWorldsAssuranceClass;
        bool provableWorldsEnabled = false;
        std::wstring snapshotArtifactId;
        std::wstring cancellationSnapshotArtifactId;
        WorkerXvmProgramReadTarget resumeTarget;
        std::wstring resumeSnapshotArtifactId;
        std::string resumeSnapshotJson;
        std::wstring expectedResumeSnapshotSha256;
        std::wstring expectedResumeExecutionId;
        std::wstring expectedResumePreviousStateSha256;
        std::wstring expectedResumeBoundInputSha256;
        uint64_t expectedResumeCheckpointSequence = 0;
        uint64_t checkpointFuel = 0;
        bool stateSnapshotEnabled = false;
        bool stateResumeEnabled = false;
    };

    using WorkerXvmProgramResolver = std::function<WorkerXvmProgramReadTarget(std::wstring const& artifactId)>;
    using WorkerXvmProgramTextReader = std::function<std::string(std::filesystem::path const& path)>;

    WorkerXvmAdmission WorkerAdmitXvmProgram(
        winrt::Windows::Data::Json::JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader);
}
