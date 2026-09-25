#include "pch.h"
#include "WorkerXvmAdmission.h"

#include "WorkerXvmIsa.h"
#include "WorkerGpuXvmProvableWorldsProfile.h"
#include "WorkerXvmSnapshot.h"
#include "WorkerXvmVerifier.h"

using namespace winrt;
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
        constexpr uint64_t MaxXvmSnapshotBytesValue = 20ull * 1024ull * 1024ull;

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

        bool IsHexLocal(std::wstring const& value, size_t expectedLength)
        {
            if (value.size() != expectedLength)
            {
                return false;
            }
            return std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
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

        std::array<uint32_t, 3> ReadSpmdShape(
            JsonObject const& object,
            wchar_t const* name,
            char const* code)
        {
            if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() != JsonValueType::Array)
            {
                throw WorkerXvmError(code, "SPMD shape must be a three-element integer array");
            }
            auto values = object.GetNamedArray(name);
            if (values.Size() != 3)
            {
                throw WorkerXvmError(code, "SPMD shape must contain exactly three dimensions");
            }
            std::array<uint32_t, 3> shape{};
            for (uint32_t index = 0; index < 3; ++index)
            {
                auto value = values.GetAt(index);
                auto number = value.ValueType() == JsonValueType::Number ? value.GetNumber() : -1.0;
                if (!std::isfinite(number) || number < 1 || number > UINT32_MAX || std::floor(number) != number)
                {
                    throw WorkerXvmError(code, "SPMD dimensions must be positive exact u32 values");
                }
                shape[index] = static_cast<uint32_t>(number);
            }
            return shape;
        }
    }

    WorkerXvmAdmission WorkerAdmitXvmProgram(
        JsonObject const& node,
        WorkerXvmProgramResolver const& resolver,
        WorkerXvmProgramTextReader const& textReader)
    {
        auto programArtifactId = std::wstring(node.GetNamedString(L"program_artifact_id", L"").c_str());
        auto expectedProgramSha256 = LowerAsciiLocal(std::wstring(node.GetNamedString(L"expected_program_sha256", L"").c_str()));
        auto isaVersion = std::wstring(node.GetNamedString(L"isa_version", WorkerXvmV1Spec().isaVersion).c_str());
        auto backend = std::wstring(node.GetNamedString(L"backend", XvmCpuReferenceBackendValue).c_str());
        if (!IsSafeIdLocal(programArtifactId))
        {
            throw WorkerXvmError("xvm.program_artifact_id_invalid", "program_artifact_id must be a safe artifact id");
        }
        if (!IsHexLocal(expectedProgramSha256, 64))
        {
            throw WorkerXvmError("xvm.expected_program_sha256_invalid", "expected_program_sha256 must be 64 hexadecimal characters");
        }
        auto isaSpec = WorkerFindXvmIsaSpec(isaVersion);
        if (isaSpec == nullptr)
        {
            throw WorkerXvmError(
                "xvm.isa_version_invalid",
                "xvm_program requires an admitted ISA version",
                {
                    "graph_admission",
                    "isa_version",
                    "an isa_version returned by describe_xvm_isa",
                    winrt::to_string(winrt::hstring(isaVersion)),
                    "refresh describe_xvm_isa, regenerate the artifact, and resubmit",
                    false,
                });
        }
        if (backend != XvmCpuReferenceBackendValue && backend != XvmCpuGpuDifferentialBackendValue &&
            backend != XvmCpuSpmdReferenceBackendValue &&
            backend != XvmCpuGpuSpmdDifferentialBackendValue &&
            backend != XvmCpuPlanCodeletDifferentialBackendValue &&
            backend != WorkerXvmCpuCapsuleBackendValue)
        {
            throw WorkerXvmError(
                "xvm.backend_not_admitted",
                "xvm_program admits cpu_reference, cpu_plan_codelet_differential, cpu_packaged_capsule_hot_kernel_differential, cpu_spmd_reference, cpu_gpu_differential or cpu_gpu_spmd_differential backends");
        }

        std::array<uint32_t, 3> spmdGridShape{};
        std::array<uint32_t, 3> spmdWorkgroupShape{};
        auto spmdBackend = backend == XvmCpuSpmdReferenceBackendValue ||
            backend == XvmCpuGpuSpmdDifferentialBackendValue;
        if (spmdBackend)
        {
            if (!node.HasKey(L"spmd_execution") ||
                node.GetNamedValue(L"spmd_execution").ValueType() != JsonValueType::Object)
            {
                throw WorkerXvmError("xvm.spmd_contract_invalid", "SPMD backends require an explicit spmd_execution object");
            }
            auto spmd = node.GetNamedObject(L"spmd_execution");
            if (spmd.Size() != 3 ||
                std::wstring(spmd.GetNamedString(L"schema_version", L"").c_str()) != L"xvm-spmd-execution-v1")
            {
                throw WorkerXvmError("xvm.spmd_contract_invalid", "spmd_execution must use the exact xvm-spmd-execution-v1 contract");
            }
            spmdGridShape = ReadSpmdShape(spmd, L"grid_shape", "xvm.spmd_grid_invalid");
            spmdWorkgroupShape = ReadSpmdShape(spmd, L"workgroup_shape", "xvm.spmd_workgroup_invalid");
        }
        else if (node.HasKey(L"spmd_execution"))
        {
            throw WorkerXvmError("xvm.spmd_backend_required", "spmd_execution is admitted only by an explicit CPU or CPU/GPU SPMD backend");
        }

        auto graphCapabilities = WorkerReadXvmCapabilities(node, L"capabilities", true);
        auto limits = WorkerGraphReadNodeResourceLimits(node, true);
        std::wstring snapshotArtifactId;
        std::wstring cancellationSnapshotArtifactId;
        uint64_t checkpointFuel = 0;
        auto stateSnapshotEnabled = node.HasKey(L"state_snapshot_policy");
        if (stateSnapshotEnabled)
        {
            auto policyValue = node.GetNamedValue(L"state_snapshot_policy");
            if (policyValue.ValueType() != JsonValueType::Object)
            {
                throw WorkerXvmError("xvm.snapshot_policy_invalid", "state_snapshot_policy must be an object");
            }
            auto policy = policyValue.GetObject();
            auto hasCancellationArtifact = policy.HasKey(L"cancellation_snapshot_artifact_id");
            if ((policy.Size() != 3 && policy.Size() != 4) ||
                !policy.HasKey(L"schema_version") || !policy.HasKey(L"checkpoint_fuel") ||
                !policy.HasKey(L"snapshot_artifact_id") || (policy.Size() == 4) != hasCancellationArtifact ||
                std::wstring(policy.GetNamedString(L"schema_version", L"").c_str()) != L"xvm-state-snapshot-policy-v2")
            {
                throw WorkerXvmError("xvm.snapshot_policy_invalid", "state_snapshot_policy must use the exact v2 contract");
            }
            auto checkpointValue = policy.GetNamedValue(L"checkpoint_fuel");
            auto checkpointNumber = checkpointValue.ValueType() == JsonValueType::Number
                ? checkpointValue.GetNumber()
                : -1.0;
            if (!std::isfinite(checkpointNumber) || checkpointNumber < 1 ||
                checkpointNumber > static_cast<double>(UINT64_MAX) || std::floor(checkpointNumber) != checkpointNumber)
            {
                throw WorkerXvmError("xvm.snapshot_checkpoint_invalid", "checkpoint_fuel must be a positive exact integer");
            }
            checkpointFuel = static_cast<uint64_t>(checkpointNumber);
            snapshotArtifactId = std::wstring(policy.GetNamedString(L"snapshot_artifact_id", L"").c_str());
            cancellationSnapshotArtifactId = std::wstring(policy.GetNamedString(L"cancellation_snapshot_artifact_id", L"").c_str());
            if (!IsSafeIdLocal(snapshotArtifactId) || checkpointFuel >= limits.fuel)
            {
                throw WorkerXvmError("xvm.snapshot_checkpoint_invalid", "snapshot artifact id and checkpoint fuel must fit the admitted node");
            }
            if (isaVersion != WorkerXvmV2Spec().isaVersion ||
                (backend != XvmCpuReferenceBackendValue &&
                 backend != XvmCpuPlanCodeletDifferentialBackendValue &&
                 backend != WorkerXvmCpuCapsuleBackendValue))
            {
                throw WorkerXvmError("xvm.snapshot_backend_not_admitted", "state snapshots are admitted only for xvm-v2 canonical, plan/codelet or packaged-capsule differential CPU execution");
            }
            if (hasCancellationArtifact && !IsSafeIdLocal(cancellationSnapshotArtifactId))
            {
                throw WorkerXvmError("xvm.snapshot_artifact_id_invalid", "cancellation snapshot artifact id must be safe");
            }
        }
        auto target = resolver(programArtifactId);
        if (!target.committed || target.artifactKind != isaSpec->artifactKind)
        {
            throw WorkerXvmError("xvm.program_artifact_invalid", "program artifact kind does not match the requested ISA version");
        }
        if (target.bytes == 0 || target.bytes > WorkerXvmProgramArtifactMaxBytesValue)
        {
            throw WorkerXvmError("xvm.program_artifact_size_invalid", "program artifact exceeds the admitted XVM program size");
        }
        if (LowerAsciiLocal(target.sha256) != expectedProgramSha256)
        {
            throw WorkerXvmError(
                "xvm.program_artifact_hash_mismatch",
                "program artifact SHA-256 does not match graph admission",
                {
                    "artifact_admission",
                    "expected_program_sha256",
                    winrt::to_string(winrt::hstring(LowerAsciiLocal(target.sha256))),
                    winrt::to_string(winrt::hstring(expectedProgramSha256)),
                    "use the SHA-256 returned by the canonical assembler for the exact uploaded bytes",
                    false,
                });
        }

        auto programJson = textReader(target.path);
        if (programJson.size() != target.bytes)
        {
            throw WorkerXvmError("xvm.program_artifact_read_mismatch", "program artifact read length does not match committed metadata");
        }
        auto program = WorkerParseAndVerifyXvmProgram(programJson);
        auto staticFuelProof = WorkerAnalyzeXvmStaticFuel(program);
        WorkerXvmCpuExecutionPlan cpuExecutionPlan;
        WorkerXvmCpuCapsulePlan cpuCapsulePlan;
        double cpuPlanBuildElapsedMs = 0.0;
        auto specializedCpuBackend =
            backend == XvmCpuPlanCodeletDifferentialBackendValue ||
            backend == WorkerXvmCpuCapsuleBackendValue;
        if (specializedCpuBackend)
        {
            auto planStarted = std::chrono::steady_clock::now();
            cpuExecutionPlan = WorkerBuildXvmCpuExecutionPlan(
                program,
                expectedProgramSha256,
                staticFuelProof.worstCaseFuel);
            cpuPlanBuildElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - planStarted).count();
        }
        if (backend == WorkerXvmCpuCapsuleBackendValue)
        {
            if (!node.HasKey(L"cpu_capsule_profile_id") ||
                !node.HasKey(L"expected_cpu_capsule_profile_contract_sha256") ||
                node.GetNamedValue(L"cpu_capsule_profile_id").ValueType() != JsonValueType::String ||
                node.GetNamedValue(L"expected_cpu_capsule_profile_contract_sha256").ValueType() != JsonValueType::String)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_profile_not_admitted",
                    "packaged capsule execution requires explicit profile id and profile-contract SHA-256 strings");
            }
            auto profileId = std::wstring(node.GetNamedString(L"cpu_capsule_profile_id").c_str());
            auto profileSha256 = LowerAsciiLocal(std::wstring(
                node.GetNamedString(L"expected_cpu_capsule_profile_contract_sha256").c_str()));
            if (profileId != WorkerXvmCpuCapsuleProfileId ||
                profileSha256 != WorkerXvmCpuCapsuleProfileContractSha256)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_profile_not_admitted",
                    "packaged capsule execution does not match the exact admitted profile contract");
            }
            if (stateSnapshotEnabled && checkpointFuel != 12345ull)
            {
                throw WorkerXvmError(
                    "xvm.cpu_capsule_checkpoint_mismatch",
                    "packaged capsule snapshots require the exact verified checkpoint fuel");
            }
            cpuCapsulePlan = WorkerBuildXvmCpuCapsulePlan(
                program,
                cpuExecutionPlan,
                expectedProgramSha256);
        }
        if (program.isaVersion != isaVersion)
        {
            throw WorkerXvmError("xvm.isa_version_invalid", "program artifact and graph node ISA versions differ");
        }
        for (auto const& capability : program.capabilities)
        {
            if (!WorkerXvmHasCapability(graphCapabilities, capability.c_str()))
            {
                throw WorkerXvmError(
                    "xvm.capability_not_granted",
                    "graph node did not grant every capability required by the admitted program",
                    {
                        "graph_admission",
                        "capabilities",
                        "program capabilities must be a subset of node capabilities",
                        "missing:" + winrt::to_string(winrt::hstring(capability)),
                        "grant the missing published capability and resubmit the unchanged artifact",
                        false,
                    });
            }
        }
        if (!spmdBackend &&
            (limits.memoryBytes < program.memoryBytes || limits.outputBytes < program.outputBytes || limits.fuel > program.maxFuel))
        {
            throw WorkerXvmError(
                "xvm.resource_contract_invalid",
                "node resource limits are incompatible with the program artifact contract",
                {
                    "graph_admission",
                    "resource_limits",
                    "memory/output at least the artifact declarations and fuel no greater than artifact max_fuel",
                    "node resource_limits are incompatible",
                    "rebuild the node resource_limits from the canonical assembler metadata",
                    false,
                });
        }
        if (!spmdBackend && limits.fuel < staticFuelProof.worstCaseFuel)
        {
            throw WorkerXvmError(
                "xvm.static_fuel_node_limit_insufficient",
                "node fuel is smaller than the verified worst-case instruction bound",
                {
                    "graph_admission",
                    "resource_limits.fuel",
                    std::to_string(staticFuelProof.worstCaseFuel),
                    std::to_string(limits.fuel),
                    "set node fuel to at least the assembler static_worst_case_fuel without exceeding artifact max_fuel",
                    false,
                });
        }
        WorkerXvmSpmdExecutionPlan spmdExecutionPlan;
        if (spmdBackend)
        {
            spmdExecutionPlan = WorkerBuildXvmSpmdExecutionPlan(
                spmdGridShape,
                spmdWorkgroupShape,
                program,
                staticFuelProof,
                limits,
                expectedProgramSha256);
        }

        std::wstring provableWorldsFieldArtifactId;
        std::wstring provableWorldsVerificationArtifactId;
        std::wstring provableWorldsAssuranceClass;
        auto provableWorldsEnabled =
            spmdExecutionPlan.schemaVersion == WorkerXvmSpmdExecutionPlanSchemaVersionV3;
        if (provableWorldsEnabled)
        {
            if (backend == XvmCpuSpmdReferenceBackendValue)
            {
                if (node.HasKey(L"provable_worlds"))
                {
                    throw WorkerXvmError(
                        "xvm.provable_worlds_contract_invalid",
                        "CPU-canonical PROVABLE_WORLDS_V1 does not admit GPU lifecycle artifacts");
                }
            }
            else
            {
                if (backend != XvmCpuGpuSpmdDifferentialBackendValue ||
                    !node.HasKey(L"provable_worlds") ||
                    node.GetNamedValue(L"provable_worlds").ValueType() != JsonValueType::Object)
                {
                    throw WorkerXvmError(
                        "xvm.provable_worlds_contract_invalid",
                        "Differential PROVABLE_WORLDS_V1 requires its exact lifecycle contract");
                }
                auto world = node.GetNamedObject(L"provable_worlds");
                if (world.Size() != 5 ||
                    std::wstring(world.GetNamedString(L"schema_version", L"").c_str()) !=
                        L"provable-worlds-execution-v1" ||
                    std::wstring(world.GetNamedString(L"field_format", L"").c_str()) != L"u32_le" ||
                    std::wstring(world.GetNamedString(L"assurance_class", L"").c_str()) !=
                        L"exact_cpu_gpu_differential_v1")
                {
                    throw WorkerXvmError(
                        "xvm.provable_worlds_contract_invalid",
                        "PROVABLE_WORLDS_V1 lifecycle fields must match the versioned contract");
                }
                provableWorldsFieldArtifactId =
                    std::wstring(world.GetNamedString(L"field_artifact_id", L"").c_str());
                provableWorldsVerificationArtifactId =
                    std::wstring(world.GetNamedString(L"verification_artifact_id", L"").c_str());
                provableWorldsAssuranceClass =
                    std::wstring(world.GetNamedString(L"assurance_class", L"").c_str());
                auto resultArtifactId =
                    std::wstring(node.GetNamedString(L"result_artifact_id", L"").c_str());
                if (!IsSafeIdLocal(provableWorldsFieldArtifactId) ||
                    !IsSafeIdLocal(provableWorldsVerificationArtifactId) ||
                    provableWorldsFieldArtifactId == provableWorldsVerificationArtifactId ||
                    provableWorldsFieldArtifactId == resultArtifactId ||
                    provableWorldsVerificationArtifactId == resultArtifactId)
                {
                    throw WorkerXvmError(
                        "xvm.provable_worlds_contract_invalid",
                        "Provable Worlds field, verification and result artifact ids must be safe and distinct");
                }
            }
        }
        else if (node.HasKey(L"provable_worlds"))
        {
            throw WorkerXvmError(
                "xvm.provable_worlds_contract_invalid",
                "provable_worlds is admitted only by the exact 1024x1024 profile");
        }

        WorkerXvmProgramReadTarget resumeTarget;
        std::wstring resumeSnapshotArtifactId;
        std::string resumeSnapshotJson;
        std::wstring expectedResumeSnapshotSha256;
        std::wstring expectedResumeExecutionId;
        std::wstring expectedResumePreviousStateSha256;
        std::wstring expectedResumeBoundInputSha256;
        uint64_t expectedResumeCheckpointSequence = 0;
        auto stateResumeEnabled = node.HasKey(L"state_resume_policy");
        if (stateResumeEnabled)
        {
            auto value = node.GetNamedValue(L"state_resume_policy");
            if (value.ValueType() != JsonValueType::Object)
            {
                throw WorkerXvmError("xvm.resume_policy_invalid", "state_resume_policy must be an object");
            }
            auto policy = value.GetObject();
            if (policy.Size() != 7 || !policy.HasKey(L"schema_version") ||
                !policy.HasKey(L"snapshot_artifact_id") || !policy.HasKey(L"expected_snapshot_sha256") ||
                !policy.HasKey(L"expected_execution_id") || !policy.HasKey(L"expected_checkpoint_sequence") ||
                !policy.HasKey(L"expected_previous_state_sha256") || !policy.HasKey(L"expected_bound_input_sha256") ||
                std::wstring(policy.GetNamedString(L"schema_version", L"").c_str()) != L"xvm-state-resume-policy-v2")
            {
                throw WorkerXvmError("xvm.resume_policy_invalid", "state_resume_policy must use the exact v2 contract");
            }
            auto artifactId = std::wstring(policy.GetNamedString(L"snapshot_artifact_id", L"").c_str());
            expectedResumeSnapshotSha256 = LowerAsciiLocal(std::wstring(policy.GetNamedString(L"expected_snapshot_sha256", L"").c_str()));
            expectedResumeExecutionId = std::wstring(policy.GetNamedString(L"expected_execution_id", L"").c_str());
            expectedResumePreviousStateSha256 = LowerAsciiLocal(std::wstring(policy.GetNamedString(L"expected_previous_state_sha256", L"").c_str()));
            expectedResumeBoundInputSha256 = LowerAsciiLocal(std::wstring(policy.GetNamedString(L"expected_bound_input_sha256", L"").c_str()));
            auto sequenceValue = policy.GetNamedValue(L"expected_checkpoint_sequence");
            auto sequenceNumber = sequenceValue.ValueType() == JsonValueType::Number ? sequenceValue.GetNumber() : -1.0;
            if (!IsSafeIdLocal(artifactId) || !IsHexLocal(expectedResumeSnapshotSha256, 64) ||
                !IsSafeIdLocal(expectedResumeExecutionId) || !IsHexLocal(expectedResumePreviousStateSha256, 64) ||
                !IsHexLocal(expectedResumeBoundInputSha256, 64) ||
                !std::isfinite(sequenceNumber) || sequenceNumber < 1 ||
                sequenceNumber > static_cast<double>(UINT64_MAX) || std::floor(sequenceNumber) != sequenceNumber)
            {
                throw WorkerXvmError("xvm.resume_policy_invalid", "resume snapshot id, digest and provenance authorization must be explicit and valid");
            }
            expectedResumeCheckpointSequence = static_cast<uint64_t>(sequenceNumber);
            if (isaVersion != WorkerXvmV2Spec().isaVersion ||
                (backend != XvmCpuReferenceBackendValue &&
                 backend != XvmCpuPlanCodeletDifferentialBackendValue))
            {
                throw WorkerXvmError("xvm.resume_backend_not_admitted", "state resume is admitted only for xvm-v2 canonical or plan/codelet differential CPU execution");
            }
            resumeTarget = resolver(artifactId);
            resumeSnapshotArtifactId = artifactId;
            if (!resumeTarget.committed || resumeTarget.artifactKind != WorkerXvmStateSnapshotSchemaVersionForExecution(
                    program,
                    backend == XvmCpuPlanCodeletDifferentialBackendValue) ||
                resumeTarget.bytes == 0 || resumeTarget.bytes > MaxXvmSnapshotBytesValue)
            {
                throw WorkerXvmError("xvm.resume_snapshot_invalid", "resume snapshot is not a committed bounded XVM state artifact");
            }
            if (LowerAsciiLocal(resumeTarget.sha256) != expectedResumeSnapshotSha256)
            {
                throw WorkerXvmError("xvm.resume_snapshot_hash_mismatch", "resume snapshot SHA-256 does not match graph admission");
            }
            resumeSnapshotJson = textReader(resumeTarget.path);
            if (resumeSnapshotJson.size() != resumeTarget.bytes)
            {
                throw WorkerXvmError("xvm.resume_snapshot_read_mismatch", "resume snapshot read length does not match committed metadata");
            }
        }
        if (stateSnapshotEnabled && stateResumeEnabled)
        {
            throw WorkerXvmError("xvm.state_policy_conflict", "checkpoint publication and resume must use separate XVM jobs");
        }
        WorkerGpuXvmProfileDescriptor const* gpuProfile = nullptr;
        WorkerGpuXvmExecutionPlan gpuExecutionPlan;
        if (backend == XvmCpuGpuDifferentialBackendValue ||
            backend == XvmCpuGpuSpmdDifferentialBackendValue)
        {
            std::wstring gpuProfileId;
            if (node.HasKey(L"gpu_profile_id"))
            {
                auto gpuProfileValue = node.GetNamedValue(L"gpu_profile_id");
                if (gpuProfileValue.ValueType() != JsonValueType::String)
                {
                    throw WorkerXvmError(
                        "xvm.gpu_profile_id_invalid",
                        "gpu_profile_id must be a string identifying an admitted GPU-XVM profile");
                }
                gpuProfileId = std::wstring(gpuProfileValue.GetString().c_str());
            }
            if (gpuProfileId.empty())
            {
                auto const& legacyProfile = WorkerGpuXvmM150Profile();
                if (!WorkerGpuXvmProfileMatches(legacyProfile, program, limits))
                {
                    throw WorkerXvmError("xvm.gpu_profile_id_required", "general cpu_gpu_differential execution requires an explicit gpu_profile_id");
                }
                gpuProfile = &legacyProfile;
            }
            else
            {
                gpuProfile = WorkerFindGpuXvmProfile(gpuProfileId);
                if (gpuProfile == nullptr)
                {
                    throw WorkerXvmError("xvm.gpu_profile_id_invalid", "gpu_profile_id does not identify an admitted worker GPU-XVM profile");
                }
                if (!WorkerGpuXvmProfileMatches(*gpuProfile, program, limits))
                {
                    throw WorkerXvmError("xvm.gpu_profile_not_admitted", "program, capabilities or resource limits do not match the selected GPU-XVM profile");
                }
                if (gpuProfile->residentExecution ||
                    gpuProfile->kind == WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1)
                {
                    if (!node.HasKey(L"gpu_contract_id") ||
                        !node.HasKey(L"expected_gpu_contract_sha256") ||
                        node.GetNamedValue(L"gpu_contract_id").ValueType() != JsonValueType::String ||
                        node.GetNamedValue(L"expected_gpu_contract_sha256").ValueType() != JsonValueType::String)
                    {
                        throw WorkerXvmError(
                            "xvm.gpu_contract_mismatch",
                            "resident GPU-XVM contract id and SHA-256 must be explicit strings");
                    }
                    auto gpuContractId = std::wstring(node.GetNamedString(L"gpu_contract_id", L"").c_str());
                    auto expectedGpuContractSha256 = LowerAsciiLocal(std::wstring(
                        node.GetNamedString(L"expected_gpu_contract_sha256", L"").c_str()));
                    if (gpuContractId != gpuProfile->contractId ||
                        expectedGpuContractSha256 != gpuProfile->contractSha256)
                    {
                        throw WorkerXvmError(
                            "xvm.gpu_contract_mismatch",
                            "resident GPU-XVM execution requires the exact admitted contract id and SHA-256");
                    }
                }
            }
            if (provableWorldsEnabled &&
                (gpuProfile == nullptr ||
                 gpuProfile->kind != WorkerGpuXvmProfileKind::ProvableWorlds1024x1024U32V1))
            {
                throw WorkerXvmError(
                    "xvm.provable_worlds_contract_invalid",
                    "PROVABLE_WORLDS_V1 requires its exact GPU profile id");
            }
            if (gpuProfile != nullptr && (gpuProfile->laneParametric || gpuProfile->microtraceAdmitted))
            {
                gpuExecutionPlan = WorkerBuildGpuXvmExecutionPlan(
                    *gpuProfile,
                    program,
                    expectedProgramSha256,
                    backend == XvmCpuGpuSpmdDifferentialBackendValue
                        ? spmdExecutionPlan.planSha256
                        : std::wstring{},
                    spmdExecutionPlan.laneCount,
                    spmdExecutionPlan.gridShape,
                    spmdExecutionPlan.workgroupShape);
            }
        }

        auto backendConvergencePlan = WorkerBuildCpuGpuConvergencePlan(
            node,
            program,
            spmdExecutionPlan,
            gpuExecutionPlan,
            expectedProgramSha256);
        auto productionModePlan = WorkerBuildVerifiedProductionModePlan(
            node, backendConvergencePlan);

        WorkerXvmAdmission admission;
        admission.program = std::move(program);
        admission.staticFuelProof = staticFuelProof;
        admission.cpuExecutionPlan = std::move(cpuExecutionPlan);
        admission.cpuCapsulePlan = std::move(cpuCapsulePlan);
        admission.backendConvergencePlan = std::move(backendConvergencePlan);
        admission.productionModePlan = std::move(productionModePlan);
        admission.cpuPlanBuildElapsedMs = cpuPlanBuildElapsedMs;
        admission.target = std::move(target);
        admission.limits = limits;
        admission.expectedProgramSha256 = std::move(expectedProgramSha256);
        admission.backend = std::move(backend);
        admission.gpuProfile = gpuProfile;
        admission.gpuExecutionPlan = std::move(gpuExecutionPlan);
        admission.spmdExecutionPlan = std::move(spmdExecutionPlan);
        admission.provableWorldsFieldArtifactId = std::move(provableWorldsFieldArtifactId);
        admission.provableWorldsVerificationArtifactId =
            std::move(provableWorldsVerificationArtifactId);
        admission.provableWorldsAssuranceClass = std::move(provableWorldsAssuranceClass);
        admission.provableWorldsEnabled = provableWorldsEnabled;
        admission.snapshotArtifactId = std::move(snapshotArtifactId);
        admission.cancellationSnapshotArtifactId = std::move(cancellationSnapshotArtifactId);
        admission.resumeTarget = std::move(resumeTarget);
        admission.resumeSnapshotArtifactId = std::move(resumeSnapshotArtifactId);
        admission.resumeSnapshotJson = std::move(resumeSnapshotJson);
        admission.expectedResumeSnapshotSha256 = std::move(expectedResumeSnapshotSha256);
        admission.expectedResumeExecutionId = std::move(expectedResumeExecutionId);
        admission.expectedResumePreviousStateSha256 = std::move(expectedResumePreviousStateSha256);
        admission.expectedResumeBoundInputSha256 = std::move(expectedResumeBoundInputSha256);
        admission.expectedResumeCheckpointSequence = expectedResumeCheckpointSequence;
        admission.checkpointFuel = checkpointFuel;
        admission.stateSnapshotEnabled = stateSnapshotEnabled;
        admission.stateResumeEnabled = stateResumeEnabled;
        return admission;
    }
}
