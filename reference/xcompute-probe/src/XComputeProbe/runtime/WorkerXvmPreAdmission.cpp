#include "pch.h"
#include "WorkerXvmRuntime.h"

#include "WorkerXvmAdmission.h"
#include "WorkerXvmCpuCapsuleBackend.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmSnapshot.h"
#include "WorkerXvmVerifier.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* XvmCpuPlanCodeletDifferentialBackendValue =
            L"cpu_plan_codelet_differential";
    }

    wchar_t const* WorkerXvmIsaVersion()
    {
        return WorkerXvmV1Spec().isaVersion;
    }

    wchar_t const* WorkerXvmLatestIsaVersion()
    {
        return WorkerXvmV2Spec().isaVersion;
    }

    wchar_t const* WorkerXvmProgramArtifactSchemaVersion()
    {
        return WorkerXvmV1Spec().programSchemaVersion;
    }

    uint64_t WorkerXvmMaxProgramBytes()
    {
        return WorkerXvmProgramArtifactMaxBytesValue;
    }

    uint64_t WorkerXvmMaxInstructions()
    {
        return WorkerXvmInstructionLimit();
    }

    uint64_t WorkerXvmMaxLoopIterations()
    {
        return WorkerXvmLoopIterationLimit();
    }

    uint64_t WorkerXvmMaxLoopDepth()
    {
        return WorkerXvmLoopDepthLimit();
    }

    WorkerXvmPreAdmissionResult WorkerPreAdmitXvmProgram(
        JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerXvmSnapshotAuthorityProvider const& snapshotAuthorityLoader)
    {
        auto admission = WorkerAdmitXvmProgram(node, resolver, textReader);
        WorkerXvmPreAdmissionResult result;
        result.staticFuelProofJson =
            WorkerXvmStaticFuelProofJson(admission.staticFuelProof, admission.limits.fuel);
        result.gpuExecutionPlan = admission.gpuExecutionPlan;
        result.gpuExecutionPlanJson =
            WorkerGpuXvmExecutionPlanJson(admission.gpuExecutionPlan);
        result.spmdExecutionPlan = admission.spmdExecutionPlan;
        result.spmdExecutionPlanJson =
            WorkerXvmSpmdExecutionPlanJson(admission.spmdExecutionPlan);
        result.cpuExecutionPlan = admission.cpuExecutionPlan;
        result.cpuExecutionPlanJson =
            WorkerXvmCpuExecutionPlanJson(admission.cpuExecutionPlan);
        result.cpuCapsulePlan = admission.cpuCapsulePlan;
        result.cpuCapsulePlanJson =
            WorkerXvmCpuCapsulePlanJson(admission.cpuCapsulePlan);
        result.backendConvergencePlan = admission.backendConvergencePlan;
        result.backendConvergencePlanJson =
            WorkerCpuGpuConvergencePlanJson(admission.backendConvergencePlan);
        result.productionModePlan = admission.productionModePlan;
        result.productionModePlanJson =
            WorkerVerifiedProductionModePlanJson(admission.productionModePlan);
        result.cpuPlanBuildElapsedMs = admission.cpuPlanBuildElapsedMs;
        if (!admission.stateResumeEnabled)
        {
            return result;
        }
        if (!snapshotAuthorityLoader)
        {
            throw WorkerXvmError(
                "xvm.snapshot_authority_invalid",
                "resume pre-admission requires a read-only worker authority loader");
        }

        WorkerXvmSnapshotContext context;
        context.isaVersion = admission.program.isaVersion;
        context.programId = admission.program.programId;
        context.programSha256 = admission.expectedProgramSha256;
        context.boundInputSha256 = admission.expectedResumeBoundInputSha256;
        context.structuredControlPhaseB = WorkerXvmHasCapability(
            admission.program.capabilities,
            L"structured_control_v2_phase_b");
        if (admission.backend == XvmCpuPlanCodeletDifferentialBackendValue)
        {
            context.tieredCpuState = true;
            context.executionPlanSha256 = admission.cpuExecutionPlan.planSha256;
            context.sourceBackend = L"cpu_plan_codelet";
        }

        WorkerXvmSnapshotResumeAuthorization authorization;
        authorization.expectedExecutionId = admission.expectedResumeExecutionId;
        authorization.expectedCheckpointSequence =
            admission.expectedResumeCheckpointSequence;
        authorization.expectedPreviousStateSha256 =
            admission.expectedResumePreviousStateSha256;
        auto authority = snapshotAuthorityLoader();
        auto snapshotWide =
            std::wstring(to_hstring(admission.resumeSnapshotJson).c_str());
        auto restored = WorkerRestoreXvmStateSnapshot(
            snapshotWide,
            context,
            &authorization,
            admission.program,
            admission.limits,
            authority);
        auto canonical = WorkerXvmStateSnapshotJson(
            context,
            restored.provenance,
            restored.state,
            authority);
        if (canonical != snapshotWide ||
            restored.state.halted ||
            restored.state.fuelConsumed >= admission.limits.fuel)
        {
            throw WorkerXvmError(
                "xvm.resume_snapshot_state_invalid",
                "resume snapshot must be canonical, sealed, non-terminal and within admitted fuel");
        }
        return result;
    }

    std::wstring WorkerValidateXvmProgramAdmission(
        JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerXvmSnapshotAuthorityProvider const& snapshotAuthorityLoader)
    {
        return WorkerPreAdmitXvmProgram(
            node,
            resolver,
            textReader,
            snapshotAuthorityLoader).staticFuelProofJson;
    }
}
