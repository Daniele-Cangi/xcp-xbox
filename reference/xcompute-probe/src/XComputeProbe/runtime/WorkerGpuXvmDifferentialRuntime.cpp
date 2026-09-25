#include "pch.h"
#include "WorkerGpuXvmDifferentialRuntime.h"

#include "WorkerGpuXvmProfileContract.h"
#include "WorkerXvmInterpreter.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        std::string WideToUtf8(std::wstring const& value)
        {
            return to_string(hstring(value));
        }

        std::wstring LowerAscii(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }

        std::wstring DoubleJson(double value)
        {
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << value;
            return out.str();
        }

        WorkerGraphPublishedArtifact PublishField(
            WorkerGpuXvmDifferentialInput const& input,
            WorkerGraphArtifactPublisher const& publisher,
            std::string const& payload,
            std::wstring const& sourceBackend)
        {
            JsonObject request;
            request.Insert(
                L"artifact_id",
                JsonValue::CreateStringValue(hstring(input.provableWorldsFieldArtifactId)));
            request.Insert(
                L"artifact_kind",
                JsonValue::CreateStringValue(L"provable-worlds-field-u32-v1"));
            auto manifest = std::wstring(
                L",\"job_output_schema\":\"provable-worlds-field-u32-v1\"") +
                L",\"artifact_lifecycle_state\":\"generated_provisional\"" +
                L",\"field_shape\":[1024,1024,1]" +
                L",\"field_format\":\"u32_le\"" +
                L",\"logical_lane_count\":1048576" +
                L",\"source_backend\":" + JsonString(sourceBackend) +
                L",\"assurance_class\":\"unverified_candidate_provisional_v1\"" +
                L",\"authoritative\":false";
            return publisher(
                request,
                input.provableWorldsFieldArtifactId,
                L"provable-worlds-field-u32-v1",
                payload,
                manifest);
        }

        WorkerGraphPublishedArtifact PublishReceipt(
            WorkerGpuXvmDifferentialInput const& input,
            WorkerGraphArtifactPublisher const& publisher,
            std::wstring const& receipt,
            std::wstring const& state,
            std::wstring const& assuranceClass,
            bool authoritative)
        {
            JsonObject request;
            request.Insert(
                L"artifact_id",
                JsonValue::CreateStringValue(hstring(input.provableWorldsVerificationArtifactId)));
            request.Insert(
                L"artifact_kind",
                JsonValue::CreateStringValue(L"provable-worlds-verification-v1"));
            auto manifest = std::wstring(
                L",\"job_output_schema\":\"provable-worlds-verification-v1\"") +
                L",\"artifact_lifecycle_state\":" + JsonString(state) +
                L",\"assurance_class\":" + JsonString(assuranceClass) +
                L",\"authoritative\":" + (authoritative ? L"true" : L"false");
            return publisher(
                request,
                input.provableWorldsVerificationArtifactId,
                L"provable-worlds-verification-v1",
                WideToUtf8(receipt),
                manifest);
        }

        std::wstring BuildProvableWorldsResult(
            WorkerGpuXvmDifferentialInput const& input,
            WorkerGraphPublishedArtifact const& field,
            WorkerGraphPublishedArtifact const& receipt,
            std::wstring const& fieldSha256,
            std::wstring const& assuranceClass,
            std::wstring const& selectedBackend,
            bool productionAuthority,
            bool gpuAuthority)
        {
            auto finalState = productionAuthority ? L"verified_authoritative" : L"verified";
            auto verificationMethod = productionAuthority
                ? L"full_byte_comparison_to_cpu_reference"
                : L"full_cpu_reference_byte_comparison";
            return std::wstring(L"{\"enabled\":true,\"schema_version\":\"provable-worlds-result-v1\"") +
                L",\"field_shape\":[1024,1024,1]" +
                L",\"logical_lane_count\":1048576" +
                L",\"field_format\":\"u32_le\"" +
                L",\"field_artifact_id\":" + JsonString(field.artifactId) +
                L",\"field_sha256\":" + JsonString(fieldSha256) +
                L",\"field_bytes\":" + std::to_wstring(field.bytes) +
                L",\"verification_artifact_id\":" + JsonString(receipt.artifactId) +
                L",\"verification_artifact_sha256\":" + JsonString(receipt.sha256) +
                L",\"lifecycle\":[\"generated_provisional\",\"verification_pending\"," +
                    JsonString(finalState) + L"]" +
                L",\"final_state\":" + JsonString(finalState) +
                L",\"assurance_class\":" + JsonString(assuranceClass) +
                L",\"verification_method\":" + JsonString(verificationMethod) +
                L",\"selected_backend\":" + JsonString(selectedBackend) +
                L",\"cpu_reference_elapsed_ms\":" + DoubleJson(input.cpuReferenceElapsedMs) +
                L",\"production_authority\":" + (productionAuthority ? L"true" : L"false") +
                L",\"gpu_production_authority\":" + (gpuAuthority ? L"true" : L"false") + L"}";
        }

        WorkerGpuXvmDifferentialResult RunFallback(
            WorkerGpuXvmDifferentialInput const& input,
            WorkerGraphArtifactPublisher const& publisher,
            std::function<std::wstring(std::string const&)> const& sha256Text,
            bool gpuAttempted,
            bool gpuDeviceLost,
            bool controlledFault,
            bool actualDeviceRemoved)
        {
            if (input.convergence == nullptr || input.convergence->verifiedField.empty())
            {
                throw WorkerXvmError(
                    "xvm.production_mode_authority_denied",
                    "verified CPU capsule output is unavailable for Production Mode fallback");
            }
            auto payload = std::string(
                input.convergence->verifiedField.begin(), input.convergence->verifiedField.end());
            auto fieldSha256 = LowerAscii(sha256Text(payload));
            if (fieldSha256 != input.cpuFieldSha256)
            {
                throw WorkerXvmError(
                    "xvm.production_mode_authority_denied",
                    "verified CPU capsule fallback no longer matches the canonical field");
            }
            auto field = PublishField(
                input, publisher, payload, L"cpu_packaged_capsule_provable_worlds");
            if (LowerAscii(field.sha256) != fieldSha256)
            {
                throw WorkerXvmError(
                    "xvm.provable_worlds_publish_hash_mismatch",
                    "published capsule fallback hash differs from its verified field");
            }
            auto receiptJson = std::wstring(
                L"{\"schema_version\":\"provable-worlds-verification-v1\"") +
                L",\"field_artifact_id\":" + JsonString(field.artifactId) +
                L",\"field_sha256\":" + JsonString(fieldSha256) +
                L",\"assurance_schema\":\"xvm-assurance-class-v1\"" +
                L",\"assurance_class\":\"exact_cpu_capsule_differential_v1\"" +
                L",\"verification_method\":\"full_byte_comparison_to_cpu_reference\"" +
                L",\"cpu_field_sha256\":" + JsonString(input.cpuFieldSha256) +
                L",\"capsule_field_sha256\":" + JsonString(fieldSha256) +
                L",\"gpu_field_sha256\":null" +
                L",\"matched\":true,\"final_state\":\"verified_authoritative\"" +
                L",\"selected_backend\":\"cpu_packaged_capsule_provable_worlds\"}";
            auto receipt = PublishReceipt(
                input, publisher, receiptJson, L"verified_authoritative",
                L"exact_cpu_capsule_differential_v1", true);
            auto decision = WorkerSelectVerifiedProductionModeAuthority(
                input.productionPlan, true, gpuAttempted, false, gpuDeviceLost,
                controlledFault, actualDeviceRemoved);

            WorkerGpuXvmDifferentialResult result;
            result.gpuAttempted = gpuAttempted;
            result.gpuDeviceLost = gpuDeviceLost;
            result.gpuDifferentialJson = gpuDeviceLost
                ? std::wstring(L"{\"enabled\":false,\"backend\":\"cpu_gpu_spmd_differential\"") +
                    L",\"failure_code\":\"xvm.gpu_device_lost\"" +
                    L",\"fallback_backend\":\"cpu_packaged_capsule_provable_worlds\"}"
                : std::wstring(L"{\"enabled\":false,\"backend\":\"cpu_gpu_spmd_differential\"") +
                    L",\"skip_reason\":\"gpu_quarantined_before_admission\"" +
                    L",\"fallback_backend\":\"cpu_packaged_capsule_provable_worlds\"}";
            result.provableWorldsJson = BuildProvableWorldsResult(
                input, field, receipt, fieldSha256, decision.assuranceClass,
                decision.selectedBackend, true, false);
            result.productionModeJson = WorkerVerifiedProductionModeResultJson(decision);
            return result;
        }
    }

    WorkerGpuXvmDifferentialResult WorkerRunGpuXvmDifferential(
        WorkerGpuXvmDifferentialInput const& input,
        WorkerGraphArtifactPublisher const& publisher,
        std::function<std::wstring(std::string const&)> const& sha256Text)
    {
        if (input.profile == nullptr || input.program == nullptr || input.limits == nullptr ||
            input.executionPlan == nullptr || input.cpuRun == nullptr)
        {
            throw WorkerXvmError(
                "xvm.gpu_execution_failed",
                "GPU differential runtime requires complete admitted input");
        }
        auto production = input.productionPlan.admitted;
        if (production && input.convergence == nullptr)
        {
            throw WorkerXvmError(
                "xvm.production_mode_prerequisite_missing",
                "Production Mode execution requires verified convergence state");
        }
        auto injectDeviceLoss =
            WorkerVerifiedProductionModeShouldInjectGpuDeviceLoss(input.productionPlan);
        if (production && (!input.productionPlan.gpuEligibleAtAdmission || injectDeviceLoss))
        {
            if (injectDeviceLoss)
            {
                WorkerVerifiedProductionModeQuarantineGpu();
            }
            return RunFallback(
                input, publisher, sha256Text, false, injectDeviceLoss,
                injectDeviceLoss, false);
        }

        WorkerGpuXvmExecutionResult gpu;
        try
        {
            gpu = WorkerExecuteGpuXvmProfile(
                *input.profile, *input.program, *input.limits, *input.executionPlan,
                input.inputs, input.cancelRequested);
        }
        catch (WorkerXvmError const& error)
        {
            if (!production || error.code != "xvm.gpu_device_lost")
            {
                throw;
            }
            WorkerVerifiedProductionModeQuarantineGpu();
            return RunFallback(input, publisher, sha256Text, true, true, false, true);
        }

        WorkerGpuXvmDifferentialResult result;
        result.gpuAttempted = true;
        auto gpuOutputHex = input.provableWorlds
            ? std::wstring{}
            : WorkerXvmBytesHex(gpu.run.output);
        auto gpuFieldPayload = input.provableWorlds
            ? std::string(gpu.run.output.begin(), gpu.run.output.end())
            : std::string{};
        auto gpuFieldSha256 = input.provableWorlds
            ? LowerAscii(sha256Text(gpuFieldPayload))
            : std::wstring{};
        WorkerGraphPublishedArtifact field;
        if (input.provableWorlds)
        {
            field = PublishField(input, publisher, gpuFieldPayload, L"gpu_xvm_provable_worlds");
            if (LowerAscii(field.sha256) != gpuFieldSha256)
            {
                throw WorkerXvmError(
                    "xvm.provable_worlds_publish_hash_mismatch",
                    "published provisional field hash differs from GPU readback");
            }
        }

        auto outputMatches = input.cpuRun->output == gpu.run.output;
        auto controlMatches = input.cpuRun->controlToken == gpu.run.controlToken;
        auto fuelMatches = input.cpuRun->fuelConsumed == gpu.run.fuelConsumed;
        auto trapMatches = input.cpuRun->trap.code == gpu.run.trap.code &&
            input.cpuRun->trap.trapPc == gpu.run.trap.trapPc &&
            input.cpuRun->trap.recoveryPc == gpu.run.trap.recoveryPc &&
            input.cpuRun->trap.occurrenceCount == gpu.run.trap.occurrenceCount;
        auto exact = outputMatches && controlMatches && fuelMatches && trapMatches;
        if (!exact)
        {
            if (input.provableWorlds)
            {
                auto receiptJson = std::wstring(
                    L"{\"schema_version\":\"provable-worlds-verification-v1\"") +
                    L",\"field_artifact_id\":" + JsonString(field.artifactId) +
                    L",\"field_sha256\":" + JsonString(gpuFieldSha256) +
                    L",\"assurance_class\":\"exact_cpu_gpu_differential_v1\"" +
                    L",\"verification_method\":\"full_cpu_reference_byte_comparison\"" +
                    L",\"cpu_field_sha256\":" + JsonString(input.cpuFieldSha256) +
                    L",\"gpu_field_sha256\":" + JsonString(gpuFieldSha256) +
                    L",\"matched\":false,\"final_state\":\"quarantined\"}";
                PublishReceipt(
                    input, publisher, receiptJson, L"quarantined",
                    L"exact_cpu_gpu_differential_v1", false);
            }
            if (production)
            {
                WorkerVerifiedProductionModeQuarantineGpu();
                throw WorkerXvmError(
                    "xvm.production_mode_gpu_mismatch",
                    "GPU candidate mismatched and was quarantined before production authority");
            }
            if (input.provableWorlds)
            {
                throw WorkerXvmError(
                    "xvm.provable_worlds_differential_mismatch",
                    "Provable Worlds field failed exact CPU/GPU verification");
            }
            throw WorkerXvmError(
                input.gpuSpmdDifferential
                    ? "xvm.gpu_spmd_differential_mismatch"
                    : (input.gpuMicrotraceDifferential
                        ? "xvm.gpu_microtrace_differential_mismatch"
                        : "xvm.gpu_differential_mismatch"),
                "CPU reference and selected GPU XVM profile results diverged");
        }

        result.gpuDifferential = true;
        result.gpuExact = true;
        WorkerGraphPublishedArtifact receipt;
        auto selectedBackend = std::wstring(L"gpu_xvm_provable_worlds");
        auto assuranceClass = input.provableWorldsAssuranceClass;
        auto productionAuthority = false;
        if (production)
        {
            auto decision = WorkerSelectVerifiedProductionModeAuthority(
                input.productionPlan, true, true, true, false, false, false);
            selectedBackend = decision.selectedBackend;
            assuranceClass = decision.assuranceClass;
            productionAuthority = decision.productionAuthority;
            result.productionModeJson = WorkerVerifiedProductionModeResultJson(decision);
        }
        if (input.provableWorlds)
        {
            auto finalState = productionAuthority ? L"verified_authoritative" : L"verified";
            auto verificationMethod = productionAuthority
                ? L"full_byte_comparison_to_cpu_reference"
                : L"full_cpu_reference_byte_comparison";
            auto receiptJson = std::wstring(
                L"{\"schema_version\":\"provable-worlds-verification-v1\"") +
                L",\"field_artifact_id\":" + JsonString(field.artifactId) +
                L",\"field_sha256\":" + JsonString(gpuFieldSha256) +
                L",\"field_bytes\":" + std::to_wstring(field.bytes) +
                L",\"assurance_schema\":\"xvm-assurance-class-v1\"" +
                L",\"assurance_class\":" + JsonString(assuranceClass) +
                L",\"verification_method\":" + JsonString(verificationMethod) +
                L",\"cpu_field_sha256\":" + JsonString(input.cpuFieldSha256) +
                L",\"gpu_field_sha256\":" + JsonString(gpuFieldSha256) +
                L",\"matched\":true,\"final_state\":" + JsonString(finalState) +
                L",\"selected_backend\":" + JsonString(selectedBackend) +
                L",\"production_authority\":" + (productionAuthority ? L"true" : L"false") + L"}";
            receipt = PublishReceipt(
                input, publisher, receiptJson, finalState, assuranceClass, true);
            result.provableWorldsJson = BuildProvableWorldsResult(
                input, field, receipt, gpuFieldSha256, assuranceClass, selectedBackend,
                productionAuthority, productionAuthority);
        }

        auto recoveredTrapDifferential = input.executionPlan->schemaVersion ==
            WorkerGpuXvmExecutionPlanSchemaVersionV5;
        auto cpuTrapJson = std::wstring(L"{\"schema_version\":\"xvm-trap-state-v1\"") +
            L",\"code\":" + std::to_wstring(input.cpuRun->trap.code) +
            L",\"trap_pc\":" + std::to_wstring(input.cpuRun->trap.trapPc) +
            L",\"recovery_pc\":" + std::to_wstring(input.cpuRun->trap.recoveryPc) +
            L",\"occurrence_count\":" + JsonString(std::to_wstring(input.cpuRun->trap.occurrenceCount)) + L"}";
        auto gpuTrapJson = std::wstring(L"{\"schema_version\":\"xvm-trap-state-v1\"") +
            L",\"code\":" + std::to_wstring(gpu.run.trap.code) +
            L",\"trap_pc\":" + std::to_wstring(gpu.run.trap.trapPc) +
            L",\"recovery_pc\":" + std::to_wstring(gpu.run.trap.recoveryPc) +
            L",\"occurrence_count\":" + JsonString(std::to_wstring(gpu.run.trap.occurrenceCount)) + L"}";
        std::wstring differentialPayloadJson;
        if (input.provableWorlds)
        {
            differentialPayloadJson =
                std::wstring(L",\"cpu_field_sha256\":") +
                JsonString(input.cpuFieldSha256) +
                L",\"gpu_field_sha256\":" + JsonString(gpuFieldSha256) +
                L",\"field_bytes\":4194304";
        }
        else
        {
            differentialPayloadJson =
                std::wstring(L",\"cpu_output_hex\":") + JsonString(input.cpuOutputHex) +
                L",\"gpu_output_hex\":" + JsonString(gpuOutputHex);
        }
        result.gpuDifferentialJson =
            std::wstring(L"{\"enabled\":true,\"backend\":") +
            JsonString(std::wstring(input.gpuSpmdDifferential
                ? L"cpu_gpu_spmd_differential" : L"cpu_gpu_differential")) +
            L",\"gpu_profile_id\":" + JsonString(input.profile->profileId) +
            L",\"shader\":" + JsonString(input.profile->shaderName) +
            L",\"profile_contract_schema\":" + JsonString(input.profile->schemaVersion) +
            L",\"profile_contract_id\":" + JsonString(input.profile->contractId) +
            L",\"profile_contract_sha256\":" + JsonString(input.profile->contractSha256) +
            L",\"shader_sha256\":" + JsonString(input.profile->shaderSha256) +
            differentialPayloadJson +
            L",\"cpu_control_token\":" + JsonString(input.cpuRun->controlToken) +
            L",\"gpu_control_token\":" + JsonString(gpu.run.controlToken) +
            L",\"cpu_fuel\":" + std::to_wstring(input.cpuRun->fuelConsumed) +
            L",\"gpu_fuel\":" + std::to_wstring(gpu.run.fuelConsumed) +
            (recoveredTrapDifferential
                ? std::wstring(L",\"cpu_trap_state\":") + cpuTrapJson +
                    L",\"gpu_trap_state\":" + gpuTrapJson +
                    L",\"trap_matched\":true"
                : L"") +
            L",\"matched\":true" +
            L",\"spmd_aggregate_matched\":" +
                (input.gpuSpmdDifferential ? L"true" : L"false") +
            L",\"telemetry\":" + gpu.telemetryJson + L"}";

        if (input.gpuMicrotraceDifferential)
        {
            result.microtraceExecutionJson = std::wstring(L"{\"enabled\":true,\"schema_version\":") +
                JsonString(input.gpuMicrotraceOptimized
                    ? L"xvm-gpu-microtrace-execution-result-v2"
                    : L"xvm-gpu-microtrace-execution-result-v1") +
                L",\"execution_plan\":" + WorkerGpuXvmExecutionPlanJson(*input.executionPlan) +
                L",\"record_schema\":" + JsonString(input.executionPlan->microtraceSchemaVersion) +
                L",\"phase\":" + JsonString(input.gpuMicrotraceOptimized
                    ? L"optimized_superinstruction_v1"
                    : L"identity_source_instruction_v1") +
                L",\"source_pc_map_required\":true" +
                L",\"worker_revalidated\":true" +
                L",\"resident_shader_consumed\":true" +
                L",\"exact_source_instruction_fuel\":true" +
                L",\"functional_result_canonical\":false" +
                L",\"cpu_reference_canonical\":true" +
                L",\"gpu_authority\":false" +
                L",\"runtime_shader_compilation_used\":false}";
        }
        return result;
    }
}
