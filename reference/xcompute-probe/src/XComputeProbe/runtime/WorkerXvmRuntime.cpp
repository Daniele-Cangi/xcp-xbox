#include "pch.h"
#include "WorkerXvmRuntime.h"
#include "WorkerXvmAdmission.h"
#include "WorkerCpuGpuConvergenceBackend.h"
#include "WorkerXvmCpuTieredRuntime.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGpuXvmProfileContract.h"
#include "WorkerGpuXvmExecutor.h"
#include "WorkerGpuXvmDifferentialRuntime.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"
#include "WorkerProcessTopologyWorkloadWitness.h"
#include "WorkerXvmInterpreter.h"
#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmSnapshot.h"
#include "WorkerXvmVerifier.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* XvmCpuReferenceBackendValue = L"cpu_reference";
        constexpr wchar_t const* XvmCpuGpuDifferentialBackendValue = L"cpu_gpu_differential";
        constexpr wchar_t const* XvmCpuSpmdReferenceBackendValue = L"cpu_spmd_reference";
        constexpr wchar_t const* XvmCpuGpuSpmdDifferentialBackendValue = L"cpu_gpu_spmd_differential";
        constexpr wchar_t const* XvmCpuPlanCodeletDifferentialBackendValue = L"cpu_plan_codelet_differential";

        std::string WideToUtf8Local(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring UInt64HexLocal(uint64_t value)
        {
            std::wostringstream out;
            out << std::hex << std::setfill(L'0') << std::setw(16) << value;
            return out.str();
        }

        bool IsSafeIdLocal(std::wstring const& value)
        {
            if (value.empty() || value.size() > 64)
            {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                    (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-';
            });
        }

        std::wstring LowerAsciiLocal(std::wstring value)
        {
            for (auto& ch : value)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return value;
        }

        std::wstring DoubleJsonLocal(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << value;
            return out.str();
        }

    }

    WorkerXvmExecutionResult WorkerExecuteXvmProgram(
        WorkerXvmExecutionInput const& input,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader,
        WorkerGraphArtifactPublisher const& publisher,
        WorkerXvmSha256Text const& sha256Text,
        WorkerXvmWideMix64 const& wideMix64)
    {
        if (input.inputEdgeCount == 0 || input.boundInputSha256.empty() || input.boundInputMix64.empty())
        {
            throw WorkerXvmError("xvm.typed_input_required", "xvm_program requires at least one typed bound input edge");
        }

        auto admission = WorkerAdmitXvmProgram(input.node, resolver, textReader);
        if (input.hasAdmittedGpuExecutionPlan != admission.gpuExecutionPlan.admitted ||
            (input.hasAdmittedGpuExecutionPlan &&
                !WorkerGpuXvmExecutionPlansEqual(input.admittedGpuExecutionPlan, admission.gpuExecutionPlan)))
        {
            throw WorkerXvmError(
                "xvm.gpu_contract_mismatch",
                "GPU XVM execution plan differs from the immutable full-graph pre-admission plan");
        }
        if (input.hasAdmittedSpmdExecutionPlan != admission.spmdExecutionPlan.admitted ||
            (input.hasAdmittedSpmdExecutionPlan &&
                !WorkerXvmSpmdExecutionPlansEqual(input.admittedSpmdExecutionPlan, admission.spmdExecutionPlan)))
        {
            throw WorkerXvmError(
                "xvm.spmd_plan_mismatch",
                "SPMD execution plan differs from the immutable full-graph pre-admission plan");
        }
        if (input.hasAdmittedCpuExecutionPlan != admission.cpuExecutionPlan.admitted ||
            (input.hasAdmittedCpuExecutionPlan &&
                !WorkerXvmCpuExecutionPlansEqual(input.admittedCpuExecutionPlan, admission.cpuExecutionPlan)))
        {
            throw WorkerXvmError(
                "xvm.cpu_plan_mismatch",
                "CPU execution plan differs from the immutable full-graph pre-admission plan");
        }
        if (input.hasAdmittedCpuCapsulePlan != admission.cpuCapsulePlan.admitted ||
            (input.hasAdmittedCpuCapsulePlan &&
                !WorkerXvmCpuCapsulePlansEqual(input.admittedCpuCapsulePlan, admission.cpuCapsulePlan)))
        {
            throw WorkerXvmError(
                "xvm.cpu_capsule_plan_mismatch",
                "CPU capsule plan differs from immutable full-graph pre-admission");
        }
        WorkerValidateCpuGpuConvergencePlan(
            input.admittedBackendConvergencePlan,
            input.hasAdmittedBackendConvergencePlan,
            admission.backendConvergencePlan);
        WorkerValidateVerifiedProductionModePlan(
            input.admittedProductionModePlan,
            input.hasAdmittedProductionModePlan,
            admission.productionModePlan);
        auto const& program = admission.program;
        auto const& limits = admission.limits;
        auto const& target = admission.target;
        auto const& expectedProgramSha256 = admission.expectedProgramSha256;
        auto const& backend = admission.backend;
        auto cpuTieredBackend = backend == XvmCpuPlanCodeletDifferentialBackendValue;
        auto cpuCapsuleBackend = backend == WorkerXvmCpuCapsuleBackendValue;
        auto staticFuelProofJson = WorkerXvmStaticFuelProofJson(admission.staticFuelProof, limits.fuel);
        auto computeKind = backend == XvmCpuGpuDifferentialBackendValue ||
            backend == XvmCpuGpuSpmdDifferentialBackendValue
            ? std::wstring(admission.gpuProfile->computeKind)
            : (backend == XvmCpuSpmdReferenceBackendValue
                ? std::wstring(L"xvm_spmd_cpu_canonical_v1")
                : (cpuTieredBackend
                    ? std::wstring(L"xvm_cpu_plan_codelet_differential_v1")
                    : (cpuCapsuleBackend
                        ? std::wstring(L"xvm_cpu_packaged_capsule_hot_kernel_differential_v1")
                        : program.computeKind)));

        auto inputWords = WorkerBuildXvmInputWords(
            input.boundInputSha256, input.boundInputMix64, input.inputControlPass);
        WorkerXvmSnapshotContext snapshotContext;
        snapshotContext.isaVersion = program.isaVersion;
        snapshotContext.programId = program.programId;
        snapshotContext.programSha256 = expectedProgramSha256;
        snapshotContext.boundInputSha256 = LowerAsciiLocal(input.boundInputSha256);
        snapshotContext.structuredControlPhaseB = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2_phase_b");
        if (cpuTieredBackend)
        {
            snapshotContext.tieredCpuState = true;
            snapshotContext.executionPlanSha256 = admission.cpuExecutionPlan.planSha256;
            snapshotContext.sourceBackend = L"cpu_plan_codelet";
        }
        else if (cpuCapsuleBackend)
        {
            snapshotContext.packagedCapsuleCpuState = true;
            snapshotContext.executionPlanSha256 = admission.cpuExecutionPlan.planSha256;
            snapshotContext.sourceBackend = L"cpu_packaged_capsule_hot_kernel";
            snapshotContext.profileContractSha256 = admission.cpuCapsulePlan.profileContractSha256;
            snapshotContext.moduleSha256 = admission.cpuCapsulePlan.moduleSha256;
        }

        auto machineLimits = limits;
        if (admission.spmdExecutionPlan.admitted)
        {
            machineLimits.fuel = program.maxFuel;
            machineLimits.memoryBytes = program.memoryBytes;
            machineLimits.outputBytes = program.outputBytes;
        }
        auto machineState = WorkerCreateXvmMachineState(machineLimits);
        WorkerXvmSnapshotAuthority snapshotAuthority;
        WorkerXvmSnapshotProvenance nextSnapshotProvenance;
        auto stateTransferEnabled = admission.stateSnapshotEnabled || admission.stateResumeEnabled;
        if (stateTransferEnabled)
        {
            if (!IsSafeIdLocal(input.executionId) || !input.snapshotAuthorityProvider)
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "XVM state transfer requires a safe execution id and worker authority provider");
            }
            snapshotAuthority = input.snapshotAuthorityProvider();
            if (!snapshotAuthority.Ready())
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "XVM state transfer worker authority is unavailable");
            }
            nextSnapshotProvenance.executionId = input.executionId;
            nextSnapshotProvenance.checkpointSequence = 1;
            nextSnapshotProvenance.previousStateSha256 = LowerAsciiLocal(sha256Text(
                WideToUtf8Local(WorkerXvmMachineStateCanonicalJson(snapshotContext, machineState))));
        }

        auto stateResumeJson = std::wstring(L"{\"enabled\":false,\"schema_version\":\"xvm-state-resume-policy-v2\"}");
        if (admission.stateResumeEnabled)
        {
            if (admission.resumeSnapshotArtifactId == input.resultArtifactId)
            {
                throw WorkerXvmError("xvm.resume_snapshot_artifact_id_invalid", "resume snapshot and execution result artifact ids must differ");
            }
            auto snapshotWide = std::wstring(to_hstring(admission.resumeSnapshotJson).c_str());
            auto computedSha256 = LowerAsciiLocal(sha256Text(admission.resumeSnapshotJson));
            if (computedSha256 != admission.expectedResumeSnapshotSha256)
            {
                throw WorkerXvmError("xvm.resume_snapshot_hash_mismatch", "resume snapshot bytes do not match the admitted SHA-256");
            }
            WorkerXvmSnapshotResumeAuthorization authorization;
            authorization.expectedExecutionId = admission.expectedResumeExecutionId;
            authorization.expectedCheckpointSequence = admission.expectedResumeCheckpointSequence;
            authorization.expectedPreviousStateSha256 = admission.expectedResumePreviousStateSha256;
            auto restored = WorkerRestoreXvmStateSnapshot(
                snapshotWide,
                snapshotContext,
                &authorization,
                program,
                limits,
                snapshotAuthority);
            auto restoredCanonical = WorkerXvmStateSnapshotJson(
                snapshotContext,
                restored.provenance,
                restored.state,
                snapshotAuthority);
            machineState = std::move(restored.state);
            if (restoredCanonical != snapshotWide || machineState.halted || machineState.fuelConsumed >= limits.fuel)
            {
                throw WorkerXvmError("xvm.resume_snapshot_state_invalid", "resume snapshot must be canonical, sealed, non-terminal and within admitted fuel");
            }
            if (restored.provenance.checkpointSequence == UINT64_MAX)
            {
                throw WorkerXvmError("xvm.snapshot_provenance_invalid", "XVM snapshot checkpoint sequence cannot advance");
            }
            nextSnapshotProvenance.checkpointSequence = restored.provenance.checkpointSequence + 1;
            nextSnapshotProvenance.previousStateSha256 = computedSha256;
            stateResumeJson = std::wstring(L"{\"enabled\":true") +
                L",\"schema_version\":\"xvm-state-resume-policy-v2\"" +
                L",\"artifact_id\":" + JsonString(admission.resumeSnapshotArtifactId) +
                L",\"artifact_sha256\":" + JsonString(computedSha256) +
                L",\"source_execution_id\":" + JsonString(restored.provenance.executionId) +
                L",\"source_checkpoint_sequence\":" + std::to_wstring(restored.provenance.checkpointSequence) +
                L",\"source_previous_state_sha256\":" + JsonString(restored.provenance.previousStateSha256) +
                L",\"resumed_from_fuel\":" + std::to_wstring(machineState.fuelConsumed) +
                L",\"resumed_from_pc\":" + std::to_wstring(machineState.pc) +
                L",\"worker_seal_verified\":true" +
                L",\"authorization_verified\":true" +
                L",\"prefix_reexecuted\":false}";
        }

        auto snapshotManifestFields = [&](WorkerXvmSnapshotProvenance const& provenance,
            WorkerXvmMachineState const& state,
            std::wstring const& reason)
        {
            return std::wstring(L",\"job_output_schema\":") + JsonString(WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend)) +
                L",\"snapshot_reason\":" + JsonString(reason) +
                L",\"isa_version\":" + JsonString(program.isaVersion) +
                L",\"program_id\":" + JsonString(program.programId) +
                L",\"program_sha256\":" + JsonString(expectedProgramSha256) +
                L",\"bound_input_sha256\":" + JsonString(snapshotContext.boundInputSha256) +
                L",\"execution_id\":" + JsonString(provenance.executionId) +
                L",\"checkpoint_sequence\":" + std::to_wstring(provenance.checkpointSequence) +
                L",\"previous_state_sha256\":" + JsonString(provenance.previousStateSha256) +
                L",\"snapshot_seal_schema\":" + JsonString(WorkerXvmSnapshotSealSchemaVersion()) +
                L",\"snapshot_seal_key_id\":" + JsonString(snapshotAuthority.keyId) +
                L",\"checkpoint_fuel\":" + std::to_wstring(state.fuelConsumed) +
                L",\"checkpoint_pc\":" + std::to_wstring(state.pc);
        };

        auto throwCanceledWithSnapshot = [&](WorkerXvmMachineState const& canceledState)
        {
            if (cpuCapsuleBackend)
            {
                auto canonicalCanceledState = machineState;
                if (canceledState.fuelConsumed != 0)
                {
                    (void)WorkerAdvanceXvmCpuReference(
                        program,
                        limits,
                        inputWords,
                        canonicalCanceledState,
                        canceledState.fuelConsumed);
                }
                if (!WorkerXvmMachineStatesEqual(canonicalCanceledState, canceledState))
                {
                    WorkerQuarantineXvmCpuCapsule();
                    throw WorkerXvmError(
                        "xvm.cpu_capsule_cancellation_prefix_mismatch",
                        "packaged capsule cancellation state differs from the canonical prefix; candidate quarantined before snapshot publication");
                }
            }
            if (!admission.cancellationSnapshotArtifactId.empty())
            {
                auto canonical = WorkerXvmStateSnapshotJson(
                    snapshotContext,
                    nextSnapshotProvenance,
                    canceledState,
                    snapshotAuthority);
                auto canonicalUtf8 = WideToUtf8Local(canonical);
                auto canonicalSha256 = LowerAsciiLocal(sha256Text(canonicalUtf8));
                JsonObject publishRequest;
                publishRequest.Insert(L"artifact_id", JsonValue::CreateStringValue(hstring(admission.cancellationSnapshotArtifactId)));
                publishRequest.Insert(L"artifact_kind", JsonValue::CreateStringValue(WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend)));
                auto published = publisher(
                    publishRequest,
                    admission.cancellationSnapshotArtifactId,
                    WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend),
                    canonicalUtf8,
                    snapshotManifestFields(nextSnapshotProvenance, canceledState, L"cancellation"));
                if (LowerAsciiLocal(published.sha256) != canonicalSha256)
                {
                    throw WorkerXvmError("xvm.snapshot_publish_hash_mismatch", "published cancellation snapshot hash differs from canonical sealed state");
                }
            }
            throw WorkerXvmError("job.canceled", "XVM execution canceled at a valid instruction boundary");
        };

        auto topologyWorkloadKind =
            WorkerProcessTopologyWorkloadKind::None;
        if (admission.provableWorldsEnabled)
        {
            if (backend == XvmCpuSpmdReferenceBackendValue)
            {
                topologyWorkloadKind =
                    WorkerProcessTopologyWorkloadKind::CpuSpmdReference;
            }
            else if (backend == XvmCpuGpuSpmdDifferentialBackendValue)
            {
                topologyWorkloadKind =
                    WorkerProcessTopologyWorkloadKind::GpuVerifiedDifferential;
            }
        }
        WorkerProcessTopologyWorkloadScope topologyWorkloadWitness(
            topologyWorkloadKind);

        WorkerXvmRunResult run;
        WorkerXvmSpmdRunResult spmdRun;
        WorkerXvmCpuCapsuleDifferentialResult capsuleDifferential;
        bool capsuleDifferentialReady = false;
        double cpuReferenceElapsedMs = 0.0;
        auto cpuTieredExecutionJson = std::wstring(
            L"{\"enabled\":false,\"schema_version\":\"xvm-cpu-tiered-execution-result-v1\"}");
        auto stateSnapshotJson = std::wstring(L"{\"enabled\":false,\"schema_version\":") +
            JsonString(WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend)) + L"}";
        if (admission.stateSnapshotEnabled)
        {
            if (admission.snapshotArtifactId == input.resultArtifactId ||
                (!admission.cancellationSnapshotArtifactId.empty() &&
                    (admission.cancellationSnapshotArtifactId == input.resultArtifactId ||
                     admission.cancellationSnapshotArtifactId == admission.snapshotArtifactId)))
            {
                throw WorkerXvmError("xvm.snapshot_artifact_id_invalid", "snapshot artifact ids must be pairwise distinct from the execution result");
            }
            WorkerXvmCpuTieredCheckpointResult checkpoint;
            if (cpuCapsuleBackend)
            {
                capsuleDifferential = WorkerRunXvmCpuCapsuleDifferential(
                    program, admission.cpuExecutionPlan, admission.cpuCapsulePlan,
                    limits, inputWords, machineState, admission.checkpointFuel,
                    input.cancelRequested);
                if (capsuleDifferential.canceled)
                {
                    throwCanceledWithSnapshot(capsuleDifferential.canceledState);
                }
                capsuleDifferentialReady = true;
                checkpoint.state = capsuleDifferential.checkpointState;
                checkpoint.status = WorkerXvmAdvanceStatus::Checkpoint;
                checkpoint.elapsedMs = capsuleDifferential.checkpointElapsedMs;
            }
            else
            {
                checkpoint = WorkerRunXvmCpuTieredCheckpoint(
                    program, admission.cpuExecutionPlan, cpuTieredBackend, limits, inputWords,
                    machineState, admission.checkpointFuel, input.cancelRequested);
            }
            auto checkpointState = std::move(checkpoint.state);
            auto advanceStatus = checkpoint.status;
            auto checkpointElapsedMs = checkpoint.elapsedMs;
            if (advanceStatus == WorkerXvmAdvanceStatus::Canceled)
            {
                throwCanceledWithSnapshot(checkpointState);
            }
            if (advanceStatus != WorkerXvmAdvanceStatus::Checkpoint ||
                checkpointState.fuelConsumed != admission.checkpointFuel || checkpointState.halted)
            {
                throw WorkerXvmError("xvm.snapshot_checkpoint_unreachable", "program halted before the admitted checkpoint fuel boundary");
            }

            auto snapshotProvenance = nextSnapshotProvenance;
            auto snapshotCanonical = WorkerXvmStateSnapshotJson(
                snapshotContext,
                snapshotProvenance,
                checkpointState,
                snapshotAuthority);
            auto snapshotSha256 = LowerAsciiLocal(sha256Text(WideToUtf8Local(snapshotCanonical)));


            auto restored = WorkerRestoreXvmStateSnapshot(
                snapshotCanonical,
                snapshotContext,
                nullptr,
                program,
                limits,
                snapshotAuthority);
            auto restoredCanonical = WorkerXvmStateSnapshotJson(
                snapshotContext,
                restored.provenance,
                restored.state,
                snapshotAuthority);
            auto restoredSha256 = LowerAsciiLocal(sha256Text(WideToUtf8Local(restoredCanonical)));
            if (restoredCanonical != snapshotCanonical || restoredSha256 != snapshotSha256)
            {
                throw WorkerXvmError("xvm.snapshot_restore_hash_mismatch", "restored XVM state is not byte-identical to the content-addressed sealed snapshot");
            }
            auto restoredState = std::move(restored.state);
            if (nextSnapshotProvenance.checkpointSequence == UINT64_MAX)
            {
                throw WorkerXvmError("xvm.snapshot_provenance_invalid", "XVM snapshot checkpoint sequence cannot advance");
            }
            ++nextSnapshotProvenance.checkpointSequence;
            nextSnapshotProvenance.previousStateSha256 = snapshotSha256;
            auto resumedStatus = WorkerAdvanceXvmCpuReference(program, limits, inputWords, restoredState, 0, input.cancelRequested);
            if (resumedStatus == WorkerXvmAdvanceStatus::Canceled)
            {
                throwCanceledWithSnapshot(restoredState);
            }
            auto resumedRun = WorkerXvmMachineResult(restoredState);
            auto uninterruptedStarted = std::chrono::steady_clock::now();
            auto uninterruptedRun = WorkerRunXvmCpuReference(program, limits, inputWords);
            cpuReferenceElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uninterruptedStarted).count();
            if (resumedRun.output != uninterruptedRun.output ||
                resumedRun.controlToken != uninterruptedRun.controlToken ||
                resumedRun.fuelConsumed != uninterruptedRun.fuelConsumed ||
                resumedRun.trap.code != uninterruptedRun.trap.code ||
                resumedRun.trap.trapPc != uninterruptedRun.trap.trapPc ||
                resumedRun.trap.recoveryPc != uninterruptedRun.trap.recoveryPc ||
                resumedRun.trap.occurrenceCount != uninterruptedRun.trap.occurrenceCount)
            {
                throw WorkerXvmError("xvm.snapshot_replay_mismatch", "resumed XVM result differs from uninterrupted deterministic execution");
            }
            if (cpuTieredBackend)
            {
                auto tiered = WorkerRunXvmCpuTieredDifferential(
                    program, admission.cpuExecutionPlan, limits, inputWords,
                    WorkerCreateXvmMachineState(limits), input.cancelRequested,
                    &uninterruptedRun, cpuReferenceElapsedMs);
                if (tiered.canceled)
                {
                    throwCanceledWithSnapshot(tiered.canceledState);
                }
                cpuTieredExecutionJson = WorkerXvmCpuTieredExecutionJson(
                    admission.cpuExecutionPlan, input.admittedCpuPlanBuildElapsedMs,
                    admission.cpuPlanBuildElapsedMs, tiered, true, checkpointElapsedMs, true);
            }
            else if (cpuCapsuleBackend)
            {
                if (!capsuleDifferentialReady ||
                    !WorkerXvmRunResultsEqual(uninterruptedRun, capsuleDifferential.result))
                {
                    WorkerQuarantineXvmCpuCapsule();
                    throw WorkerXvmError(
                        "xvm.cpu_capsule_differential_mismatch",
                        "packaged capsule result did not match the canonical uninterrupted run");
                }
                cpuTieredExecutionJson =
                    WorkerXvmCpuCapsuleExecutionJson(capsuleDifferential, true);
            }
            JsonObject snapshotPublishRequest;
            snapshotPublishRequest.Insert(L"artifact_id", JsonValue::CreateStringValue(hstring(admission.snapshotArtifactId)));
            snapshotPublishRequest.Insert(L"artifact_kind", JsonValue::CreateStringValue(WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend)));
            auto snapshotPublished = publisher(
                snapshotPublishRequest,
                admission.snapshotArtifactId,
                WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend),
                WideToUtf8Local(snapshotCanonical),
                snapshotManifestFields(snapshotProvenance, checkpointState, L"checkpoint"));
            if (LowerAsciiLocal(snapshotPublished.sha256) != snapshotSha256)
            {
                throw WorkerXvmError("xvm.snapshot_publish_hash_mismatch", "published snapshot hash differs from its canonical sealed state digest");
            }
            run = std::move(resumedRun);
            stateSnapshotJson = std::wstring(L"{\"enabled\":true") +
                L",\"schema_version\":" + JsonString(WorkerXvmStateSnapshotSchemaVersionForExecution(program, cpuTieredBackend, cpuCapsuleBackend)) +
                L",\"artifact_id\":" + JsonString(snapshotPublished.artifactId) +
                L",\"artifact_sha256\":" + JsonString(snapshotSha256) +
                L",\"execution_id\":" + JsonString(snapshotProvenance.executionId) +
                L",\"checkpoint_sequence\":" + std::to_wstring(snapshotProvenance.checkpointSequence) +
                L",\"previous_state_sha256\":" + JsonString(snapshotProvenance.previousStateSha256) +
                L",\"checkpoint_fuel\":" + std::to_wstring(checkpointState.fuelConsumed) +
                L",\"checkpoint_pc\":" + std::to_wstring(checkpointState.pc) +
                L",\"worker_seal_schema\":" + JsonString(WorkerXvmSnapshotSealSchemaVersion()) +
                L",\"worker_seal_key_id\":" + JsonString(snapshotAuthority.keyId) +
                (cpuCapsuleBackend
                    ? std::wstring(L",\"execution_plan_sha256\":") + JsonString(admission.cpuExecutionPlan.planSha256) +
                        L",\"source_backend\":\"cpu_packaged_capsule_hot_kernel\"" +
                        L",\"profile_contract_sha256\":" + JsonString(admission.cpuCapsulePlan.profileContractSha256) +
                        L",\"module_sha256\":" + JsonString(admission.cpuCapsulePlan.moduleSha256)
                    : (cpuTieredBackend
                        ? std::wstring(L",\"execution_plan_sha256\":") + JsonString(admission.cpuExecutionPlan.planSha256) +
                            L",\"source_backend\":\"cpu_plan_codelet\""
                        : L"")) +
                L",\"worker_seal_verified\":true" +
                L",\"restore_digest_verified\":true" +
                L",\"final_matches_uninterrupted\":true}";
        }
        else
        {
            if (admission.spmdExecutionPlan.admitted)
            {
                auto cpuStarted = std::chrono::steady_clock::now();
                spmdRun = WorkerRunXvmSpmdCpuReference(
                    admission.spmdExecutionPlan,
                    program,
                    inputWords,
                    input.cancelRequested);
                cpuReferenceElapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - cpuStarted).count();
                run = spmdRun.aggregate;
            }
            else if (cpuCapsuleBackend)
            {
                auto capsule = WorkerRunXvmCpuCapsuleDifferential(
                    program, admission.cpuExecutionPlan, admission.cpuCapsulePlan,
                    limits, inputWords, machineState, 0, input.cancelRequested);
                if (capsule.canceled)
                {
                    throwCanceledWithSnapshot(capsule.canceledState);
                }
                run = capsule.result;
                cpuReferenceElapsedMs = capsule.referenceElapsedMs;
                cpuTieredExecutionJson = WorkerXvmCpuCapsuleExecutionJson(capsule, false);
            }
            else if (cpuTieredBackend)
            {
                auto tiered = WorkerRunXvmCpuTieredDifferential(
                    program, admission.cpuExecutionPlan, limits, inputWords,
                    machineState, input.cancelRequested);
                if (tiered.canceled)
                {
                    throwCanceledWithSnapshot(tiered.canceledState);
                }
                run = tiered.result;
                cpuReferenceElapsedMs = tiered.referenceElapsedMs;
                cpuTieredExecutionJson = WorkerXvmCpuTieredExecutionJson(
                    admission.cpuExecutionPlan, input.admittedCpuPlanBuildElapsedMs,
                    admission.cpuPlanBuildElapsedMs, tiered, false, 0.0, false);
            }
            else
            {
                auto status = WorkerAdvanceXvmCpuReference(program, limits, inputWords, machineState, 0, input.cancelRequested);
                if (status == WorkerXvmAdvanceStatus::Canceled)
                {
                    throwCanceledWithSnapshot(machineState);
                }
                run = WorkerXvmMachineResult(machineState);
            }
        }
        auto provableWorlds = admission.provableWorldsEnabled;
        auto cpuFieldPayload = provableWorlds
            ? std::string(run.output.begin(), run.output.end())
            : std::string{};
        auto cpuFieldSha256 = provableWorlds
            ? LowerAsciiLocal(sha256Text(cpuFieldPayload))
            : std::wstring{};
        WorkerCpuGpuConvergenceResult backendConvergenceResult;
        auto backendConvergenceReady = false;
        if (admission.backendConvergencePlan.admitted)
        {
            backendConvergenceResult = WorkerRunCpuGpuConvergenceCpuCandidate(
                admission.backendConvergencePlan,
                inputWords,
                run.output,
                cpuReferenceElapsedMs,
                input.cancelRequested);
            if (backendConvergenceResult.canceled)
            {
                throw WorkerXvmError(
                    "job.canceled",
                    "backend convergence canceled at a bounded CPU lane-chunk boundary");
            }
            backendConvergenceReady = true;
        }
        auto outputHex = provableWorlds ? std::wstring{} : WorkerXvmBytesHex(run.output);
        auto provableWorldsJson = std::wstring(
            L"{\"enabled\":false,\"schema_version\":\"provable-worlds-result-v1\"}");
        auto structuredControlPhaseBEnabled = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2_phase_b");
        auto trapStateJson = std::wstring(L"{\"schema_version\":\"xvm-trap-state-v1\"") +
            L",\"status\":" + JsonString(run.trap.occurrenceCount == 0 ? L"none" : L"recovered") +
            L",\"code\":" + std::to_wstring(run.trap.code) +
            L",\"trap_pc\":" + std::to_wstring(run.trap.trapPc) +
            L",\"recovery_pc\":" + std::to_wstring(run.trap.recoveryPc) +
            L",\"occurrence_count\":" + JsonString(std::to_wstring(run.trap.occurrenceCount)) + L"}";
        auto typedMemoryEnabled = WorkerXvmHasCapability(program.capabilities, L"typed_memory_v1");
        auto typedMemoryJson = std::wstring(L"{\"enabled\":") +
            (typedMemoryEnabled ? L"true" : L"false") +
            L",\"schema_version\":\"xvm-typed-memory-v1\"" +
            L",\"element_type\":\"u32\"" +
            L",\"input_view_count\":" + std::to_wstring(program.inputViews.size()) +
            L",\"memory_view_count\":" + std::to_wstring(program.memoryViews.size()) +
            L",\"output_view_count\":" + std::to_wstring(program.outputViews.size()) +
            L",\"raw_host_pointers\":false,\"host_calls\":false}";
        auto structuredControlEnabled = WorkerXvmHasCapability(program.capabilities, L"structured_control_v2");
        auto structuredControlJson = std::wstring(L"{\"enabled\":") +
            (structuredControlEnabled ? L"true" : L"false") +
            L",\"schema_version\":\"xvm-structured-control-v2\"" +
            L",\"max_depth\":8,\"cfg_verified\":true,\"region_ownership_verified\":true" +
            L",\"persistent_snapshot_stack\":false,\"legacy_branch_mix\":false" +
            (structuredControlPhaseBEnabled
                ? std::wstring(L",\"phase_b\":true,\"ordered_comparisons\":true,\"select_u32\":true") +
                    L",\"innermost_loop_control\":true,\"deterministic_recovery\":true,\"trap_state\":" + trapStateJson
                : L"") +
            L",\"host_calls\":false,\"host_effects\":false}";
        auto backendConvergenceJson = std::wstring(
            L"{\"enabled\":false,\"schema_version\":\"cpu-gpu-evolved-backend-convergence-result-v1\"}");
        auto gpuMicrotraceOptimized = admission.gpuProfile != nullptr &&
            admission.gpuProfile->kind == WorkerGpuXvmProfileKind::MicrotraceOptimizedStraightLineU32V1;
        auto gpuSpmdDifferential = backend == XvmCpuGpuSpmdDifferentialBackendValue;
        auto gpuMicrotraceDifferential = backend == XvmCpuGpuDifferentialBackendValue &&
            admission.gpuProfile != nullptr && admission.gpuProfile->microtraceAdmitted;
        WorkerGpuXvmDifferentialResult gpuResult;
        if (backend == XvmCpuGpuDifferentialBackendValue || gpuSpmdDifferential)
        {
            WorkerGpuXvmDifferentialInput gpuInput;
            gpuInput.profile = admission.gpuProfile;
            gpuInput.program = &program;
            gpuInput.limits = &limits;
            gpuInput.executionPlan = &admission.gpuExecutionPlan;
            gpuInput.cpuRun = &run;
            gpuInput.convergence = backendConvergenceReady ? &backendConvergenceResult : nullptr;
            gpuInput.productionPlan = admission.productionModePlan;
            gpuInput.inputs = inputWords;
            gpuInput.cancelRequested = input.cancelRequested;
            gpuInput.backend = backend;
            gpuInput.cpuOutputHex = outputHex;
            gpuInput.cpuFieldSha256 = cpuFieldSha256;
            gpuInput.provableWorldsFieldArtifactId = admission.provableWorldsFieldArtifactId;
            gpuInput.provableWorldsVerificationArtifactId =
                admission.provableWorldsVerificationArtifactId;
            gpuInput.provableWorldsAssuranceClass = admission.provableWorldsAssuranceClass;
            gpuInput.cpuReferenceElapsedMs = cpuReferenceElapsedMs;
            gpuInput.gpuSpmdDifferential = gpuSpmdDifferential;
            gpuInput.gpuMicrotraceDifferential = gpuMicrotraceDifferential;
            gpuInput.gpuMicrotraceOptimized = gpuMicrotraceOptimized;
            gpuInput.provableWorlds = provableWorlds;
            gpuResult = WorkerRunGpuXvmDifferential(gpuInput, publisher, sha256Text);
        }
        auto gpuDifferentialJson = gpuResult.gpuDifferentialJson;
        auto microtraceExecutionJson = gpuResult.microtraceExecutionJson;
        provableWorldsJson = gpuResult.provableWorldsJson;
        auto productionModeJson = gpuResult.productionModeJson;
        auto gpuDifferential = gpuResult.gpuDifferential;
        if (backendConvergenceReady && !admission.productionModePlan.admitted)
        {
            backendConvergenceJson = WorkerCpuGpuConvergenceResultJson(
                backendConvergenceResult, gpuDifferential);
        }
        auto spmdExecutionJson = std::wstring(L"{\"enabled\":false,\"schema_version\":\"xvm-spmd-execution-result-v1\"}");
        auto spmdExecution = admission.spmdExecutionPlan.admitted;
        if (spmdExecution)
        {
            auto firstFailLane = spmdRun.firstFailControlLane == UINT32_MAX
                ? std::wstring(L"null")
                : std::to_wstring(spmdRun.firstFailControlLane);
            auto aggregateTrap = spmdRun.aggregateTrapLane == UINT32_MAX
                ? std::wstring(L"null")
                : std::wstring(L"{\"lane_id\":") + std::to_wstring(spmdRun.aggregateTrapLane) +
                    L",\"code\":" + std::to_wstring(run.trap.code) +
                    L",\"source_pc\":" + std::to_wstring(run.trap.trapPc) +
                    L",\"recovery_pc\":" + std::to_wstring(run.trap.recoveryPc) +
                    L",\"occurrence_count\":" + JsonString(std::to_wstring(run.trap.occurrenceCount)) + L"}";
            spmdExecutionJson = std::wstring(L"{\"enabled\":true") +
                L",\"schema_version\":\"xvm-spmd-execution-result-v1\"" +
                L",\"execution_plan\":" + WorkerXvmSpmdExecutionPlanJson(admission.spmdExecutionPlan) +
                L",\"backend\":" + JsonString(gpuSpmdDifferential
                    ? L"cpu_gpu_spmd_differential" : L"cpu_spmd_reference") +
                L",\"canonical_backend\":true" +
                L",\"gpu_execution_admitted\":" + (gpuSpmdDifferential ? L"true" : L"false") +
                L",\"gpu_differential_matched\":" + (gpuSpmdDifferential && gpuDifferential ? L"true" : L"false") +
                L",\"aggregate_control\":" + JsonString(run.controlToken) +
                L",\"first_fail_control_lane\":" + firstFailLane +
                L",\"aggregate_trap\":" + aggregateTrap +
                L",\"aggregate_fuel_consumed\":" + std::to_wstring(run.fuelConsumed) + L"}";
        }
        auto functionalCanonical = std::wstring(L"{\"schema_version\":\"xvm-functional-result-0.1\"") +
            L",\"isa_version\":" + JsonString(program.isaVersion) +
            L",\"profile\":" + JsonString(program.profile) +
            L",\"program_id\":" + JsonString(program.programId) +
            L",\"program_sha256\":" + JsonString(expectedProgramSha256) +
            L",\"bound_input_sha256\":" + JsonString(LowerAsciiLocal(input.boundInputSha256)) +
            L",\"bound_input_mix64\":" + JsonString(LowerAsciiLocal(input.boundInputMix64)) +
            (provableWorlds
                ? std::wstring(L",\"field_sha256\":") + JsonString(cpuFieldSha256) +
                    L",\"field_bytes\":4194304,\"field_shape\":[1024,1024,1],\"field_format\":\"u32_le\""
                : std::wstring(L",\"output_hex\":") + JsonString(outputHex)) +
            L",\"control_token\":" + JsonString(run.controlToken) +
            (spmdExecution ? L",\"spmd_execution\":" + spmdExecutionJson : L"") +
            (structuredControlPhaseBEnabled ? L",\"trap_state\":" + trapStateJson : L"") + L"}";
        auto logicalSha256 = sha256Text(WideToUtf8Local(functionalCanonical));
        auto computeMix64 = UInt64HexLocal(wideMix64(functionalCanonical, 0x243f6a8885a308d3ull));
        auto resourceUsageJson = WorkerGraphResourceUsageJson(run.fuelConsumed, limits.memoryBytes, run.output.size());
        auto payload = std::wstring(L"{\"schema_version\":\"xvm-execution-result-0.1\"") +
            L",\"protocol_version\":" + JsonString(input.protocolVersion) +
            L",\"node_id\":" + JsonString(input.nodeId) +
            L",\"functional_canonical\":" + functionalCanonical +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"compute_mix64\":" + JsonString(computeMix64) +
            L",\"backend\":" + JsonString(backend) +
            L",\"compute_kind\":" + JsonString(computeKind) +
            L",\"typed_memory\":" + typedMemoryJson +
            L",\"structured_control\":" + structuredControlJson +
            L",\"static_fuel_proof\":" + staticFuelProofJson +
            L",\"gpu_differential\":" + gpuDifferentialJson +
            L",\"microtrace_execution\":" + microtraceExecutionJson +
            L",\"spmd_execution\":" + spmdExecutionJson +
            L",\"provable_worlds\":" + provableWorldsJson +
            L",\"backend_convergence\":" + backendConvergenceJson +
            L",\"production_mode\":" + productionModeJson +
            L",\"cpu_tiered_execution\":" + cpuTieredExecutionJson +
            L",\"state_snapshot\":" + stateSnapshotJson +
            L",\"state_resume\":" + stateResumeJson +
            L",\"resource_usage\":" + resourceUsageJson +
            L",\"input_edges\":" + input.inputEdgesJson +
            L",\"arbitrary_native_or_host_code_executed\":false" +
            L",\"runtime_shader_compilation_used\":false}";

        JsonObject publishRequest;
        publishRequest.Insert(L"artifact_id", JsonValue::CreateStringValue(hstring(input.resultArtifactId)));
        publishRequest.Insert(L"artifact_kind", JsonValue::CreateStringValue(L"xvm-execution-result-v1"));
        auto manifestFields = std::wstring(L",\"job_output_schema\":\"xvm-execution-result-0.1\"") +
            L",\"isa_version\":" + JsonString(program.isaVersion) +
            L",\"backend\":" + JsonString(backend) +
            L",\"compute_kind\":" + JsonString(computeKind) +
            L",\"program_id\":" + JsonString(program.programId) +
            L",\"program_sha256\":" + JsonString(expectedProgramSha256) +
            L",\"logical_sha256\":" + JsonString(logicalSha256) +
            L",\"compute_mix64\":" + JsonString(computeMix64) +
            L",\"control_token\":" + JsonString(run.controlToken) +
            L",\"typed_memory_v1\":" + (typedMemoryEnabled ? L"true" : L"false") +
            L",\"structured_control_v2\":" + (structuredControlEnabled ? L"true" : L"false") +
            L",\"structured_control_v2_phase_b\":" + (structuredControlPhaseBEnabled ? L"true" : L"false") +
            L",\"trap_occurrence_count\":" + std::to_wstring(run.trap.occurrenceCount) +
            L",\"static_worst_case_fuel\":" + std::to_wstring(
                spmdExecution ? admission.spmdExecutionPlan.aggregateWorstCaseFuel : admission.staticFuelProof.worstCaseFuel) +
            L",\"spmd_execution\":" + (spmdExecution ? L"true" : L"false") +
            L",\"spmd_lane_count\":" + std::to_wstring(spmdExecution ? admission.spmdExecutionPlan.laneCount : 1) +
            L",\"state_snapshot_enabled\":" + (admission.stateSnapshotEnabled ? L"true" : L"false");
        auto published = publisher(
            publishRequest,
            input.resultArtifactId,
            L"xvm-execution-result-v1",
            WideToUtf8Local(payload),
            manifestFields);

        WorkerXvmExecutionResult result;
        result.published = published;
        result.programId = program.programId;
        result.programSha256 = expectedProgramSha256;
        result.logicalSha256 = logicalSha256;
        result.computeMix64 = computeMix64;
        result.controlToken = run.controlToken;
        result.outputHex = outputHex;
        result.backend = backend;
        result.computeKind = computeKind;
        result.functionalCanonicalJson = functionalCanonical;
        result.gpuDifferentialJson = gpuDifferentialJson;
        result.microtraceExecutionJson = microtraceExecutionJson;
        result.spmdExecutionJson = spmdExecutionJson;
        result.provableWorldsJson = provableWorldsJson;
        result.backendConvergenceJson = backendConvergenceJson;
        result.productionModeJson = productionModeJson;
        result.cpuTieredExecutionJson = cpuTieredExecutionJson;
        result.typedMemoryJson = typedMemoryJson;
        result.structuredControlJson = structuredControlJson;
        result.staticFuelProofJson = staticFuelProofJson;
        result.stateSnapshotJson = stateSnapshotJson;
        result.stateResumeJson = stateResumeJson;
        result.resourceUsageJson = resourceUsageJson;
        result.programBytes = target.bytes;
        result.instructionCount = program.words.size() / WorkerXvmInstructionWords();
        result.staticWorstCaseFuel = spmdExecution
            ? admission.spmdExecutionPlan.aggregateWorstCaseFuel
            : admission.staticFuelProof.worstCaseFuel;
        result.fuelConsumed = run.fuelConsumed;
        result.memoryBytes = limits.memoryBytes;
        result.outputBytes = run.output.size();
        result.gpuDifferential = gpuDifferential;
        result.spmdExecution = spmdExecution;
        result.ok = true;
        result.verified = true;
        return result;
    }
}
