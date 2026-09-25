#include "pch.h"
#include "WorkerVerifiedProductionMode.h"

#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe
{
    namespace
    {
        std::atomic<bool> GpuQuarantined{ false };

        std::wstring Sha256Text(std::wstring const& value)
        {
            auto provider = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
            auto buffer = CryptographicBuffer::ConvertStringToBinary(
                hstring(value), BinaryStringEncoding::Utf8);
            auto digest = std::wstring(
                CryptographicBuffer::EncodeToHexString(provider.HashData(buffer)).c_str());
            std::transform(digest.begin(), digest.end(), digest.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return digest;
        }

        std::wstring PlanMaterialJson(WorkerVerifiedProductionModePlan const& plan)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(plan.schemaVersion) +
                L",\"admitted\":true,\"immutable\":true" +
                L",\"contract_id\":" + JsonString(plan.contractId) +
                L",\"policy_id\":" + JsonString(plan.policyId) +
                L",\"policy_sha256\":" + JsonString(plan.policySha256) +
                L",\"selection_policy\":" + JsonString(plan.selectionPolicy) +
                L",\"validation_fault\":" + JsonString(plan.validationFault) +
                L",\"convergence_plan_sha256\":" + JsonString(plan.convergencePlanSha256) +
                L",\"gpu_execution_plan_sha256\":" + JsonString(plan.gpuExecutionPlanSha256) +
                L",\"capsule_module_sha256\":" + JsonString(plan.capsuleModuleSha256) +
                L",\"gpu_eligible_at_admission\":" +
                    (plan.gpuEligibleAtAdmission ? L"true" : L"false") +
                L",\"automatic_backend_selection\":true" +
                L",\"production_authority_conditional\":true}";
        }
    }

    WorkerVerifiedProductionModePlan WorkerBuildVerifiedProductionModePlan(
        JsonObject const& node,
        WorkerCpuGpuConvergencePlan const& convergencePlan)
    {
        WorkerVerifiedProductionModePlan plan;
        if (!node.HasKey(L"production_mode"))
        {
            return plan;
        }
        if (!convergencePlan.admitted)
        {
            throw WorkerXvmError(
                "xvm.production_mode_prerequisite_missing",
                "Verified Production Mode requires the exact evolved CPU/GPU convergence plan");
        }
        auto value = node.GetNamedValue(L"production_mode");
        if (value.ValueType() != JsonValueType::Object)
        {
            throw WorkerXvmError(
                "xvm.production_mode_contract_invalid",
                "production_mode must be an exact versioned object");
        }
        auto request = value.GetObject();
        auto validationFault = std::wstring(
            request.GetNamedString(L"validation_fault", L"").c_str());
        if (request.Size() != 6 ||
            std::wstring(request.GetNamedString(L"schema_version", L"").c_str()) !=
                WorkerVerifiedProductionModeRequestSchemaVersion ||
            std::wstring(request.GetNamedString(L"contract_id", L"").c_str()) !=
                WorkerVerifiedProductionModeContractId ||
            std::wstring(request.GetNamedString(L"policy_id", L"").c_str()) !=
                WorkerVerifiedProductionModePolicyId ||
            std::wstring(request.GetNamedString(L"expected_policy_sha256", L"").c_str()) !=
                WorkerVerifiedProductionModePolicySha256 ||
            std::wstring(request.GetNamedString(L"selection_policy", L"").c_str()) !=
                WorkerVerifiedProductionModeSelectionPolicy ||
            (validationFault != WorkerVerifiedProductionModeNoFault &&
             validationFault != WorkerVerifiedProductionModeControlledDeviceLoss))
        {
            throw WorkerXvmError(
                "xvm.production_mode_contract_invalid",
                "Verified Production Mode id, policy digest, selection and validation fault must match exactly");
        }

        plan.admitted = true;
        plan.gpuEligibleAtAdmission = !GpuQuarantined.load();
        plan.schemaVersion = WorkerVerifiedProductionModePlanSchemaVersion;
        plan.contractId = WorkerVerifiedProductionModeContractId;
        plan.policyId = WorkerVerifiedProductionModePolicyId;
        plan.policySha256 = WorkerVerifiedProductionModePolicySha256;
        plan.selectionPolicy = WorkerVerifiedProductionModeSelectionPolicy;
        plan.validationFault = validationFault;
        plan.convergencePlanSha256 = convergencePlan.planSha256;
        plan.gpuExecutionPlanSha256 = convergencePlan.gpuExecutionPlanSha256;
        plan.capsuleModuleSha256 = convergencePlan.moduleSha256;
        plan.planSha256 = Sha256Text(PlanMaterialJson(plan));
        return plan;
    }

    bool WorkerVerifiedProductionModePlansEqual(
        WorkerVerifiedProductionModePlan const& expected,
        WorkerVerifiedProductionModePlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            expected.gpuEligibleAtAdmission == actual.gpuEligibleAtAdmission &&
            expected.schemaVersion == actual.schemaVersion &&
            expected.contractId == actual.contractId &&
            expected.policyId == actual.policyId &&
            expected.policySha256 == actual.policySha256 &&
            expected.selectionPolicy == actual.selectionPolicy &&
            expected.validationFault == actual.validationFault &&
            expected.convergencePlanSha256 == actual.convergencePlanSha256 &&
            expected.gpuExecutionPlanSha256 == actual.gpuExecutionPlanSha256 &&
            expected.capsuleModuleSha256 == actual.capsuleModuleSha256 &&
            expected.planSha256 == actual.planSha256;
    }

    void WorkerValidateVerifiedProductionModePlan(
        WorkerVerifiedProductionModePlan const& admittedPlan,
        bool hasAdmittedPlan,
        WorkerVerifiedProductionModePlan const& rebuiltPlan)
    {
        if (hasAdmittedPlan != rebuiltPlan.admitted ||
            (hasAdmittedPlan && !WorkerVerifiedProductionModePlansEqual(admittedPlan, rebuiltPlan)))
        {
            throw WorkerXvmError(
                "xvm.production_mode_plan_mismatch",
                "Verified Production Mode plan differs from immutable full-graph pre-admission");
        }
    }

    std::wstring WorkerVerifiedProductionModePlanJson(
        WorkerVerifiedProductionModePlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        auto material = PlanMaterialJson(plan);
        material.pop_back();
        return material + L",\"plan_sha256\":" + JsonString(plan.planSha256) + L"}";
    }

    bool WorkerVerifiedProductionModeShouldInjectGpuDeviceLoss(
        WorkerVerifiedProductionModePlan const& plan) noexcept
    {
        return plan.admitted && plan.gpuEligibleAtAdmission &&
            plan.validationFault == WorkerVerifiedProductionModeControlledDeviceLoss;
    }

    void WorkerVerifiedProductionModeQuarantineGpu() noexcept
    {
        GpuQuarantined.store(true);
    }

    bool WorkerVerifiedProductionModeGpuQuarantined() noexcept
    {
        return GpuQuarantined.load();
    }

    WorkerVerifiedProductionModeDecision WorkerSelectVerifiedProductionModeAuthority(
        WorkerVerifiedProductionModePlan const& plan,
        bool cpuCapsuleExact,
        bool gpuAttempted,
        bool gpuExact,
        bool gpuDeviceLost,
        bool controlledFault,
        bool actualDeviceRemoved)
    {
        if (!plan.admitted || !cpuCapsuleExact)
        {
            throw WorkerXvmError(
                "xvm.production_mode_authority_denied",
                "production authority requires an admitted policy and exact CPU/capsule verification");
        }
        if (gpuAttempted && !gpuExact && !gpuDeviceLost)
        {
            WorkerVerifiedProductionModeQuarantineGpu();
            throw WorkerXvmError(
                "xvm.production_mode_gpu_mismatch",
                "GPU candidate mismatched the canonical result and was quarantined before authority");
        }

        WorkerVerifiedProductionModeDecision decision;
        decision.plan = plan;
        decision.cpuCapsuleExact = cpuCapsuleExact;
        decision.gpuAttempted = gpuAttempted;
        decision.gpuExact = gpuExact;
        decision.gpuDeviceLost = gpuDeviceLost;
        decision.controlledFault = controlledFault;
        decision.actualDeviceRemoved = actualDeviceRemoved;
        decision.verificationMethod = L"full_byte_comparison_to_cpu_reference";
        decision.automaticSelection = true;
        decision.productionAuthority = true;

        if (gpuExact)
        {
            decision.selectedBackend = L"gpu_xvm_provable_worlds";
            decision.assuranceClass = L"exact_cpu_capsule_gpu_differential_v1";
            decision.fallbackReason = L"none";
            decision.deviceLossObservation = L"none";
        }
        else
        {
            if (gpuDeviceLost)
            {
                WorkerVerifiedProductionModeQuarantineGpu();
            }
            decision.selectedBackend = L"cpu_packaged_capsule_provable_worlds";
            decision.assuranceClass = L"exact_cpu_capsule_differential_v1";
            decision.fallbackReason = gpuDeviceLost
                ? L"gpu_device_lost"
                : L"gpu_quarantined_before_admission";
            decision.deviceLossObservation = controlledFault
                ? L"controlled_pre_dispatch_fault_v1"
                : (actualDeviceRemoved ? L"d3d_device_removed" : L"none");
        }
        decision.gpuQuarantined = WorkerVerifiedProductionModeGpuQuarantined();
        return decision;
    }

    std::wstring WorkerVerifiedProductionModeResultJson(
        WorkerVerifiedProductionModeDecision const& decision)
    {
        return std::wstring(L"{\"enabled\":true,\"schema_version\":") +
            JsonString(WorkerVerifiedProductionModeResultSchemaVersion) +
            L",\"gate_id\":" + JsonString(WorkerVerifiedProductionModeContractId) +
            L",\"plan\":" + WorkerVerifiedProductionModePlanJson(decision.plan) +
            L",\"selected_backend\":" + JsonString(decision.selectedBackend) +
            L",\"assurance_class\":" + JsonString(decision.assuranceClass) +
            L",\"verification_method\":" + JsonString(decision.verificationMethod) +
            L",\"fallback_reason\":" + JsonString(decision.fallbackReason) +
            L",\"device_loss_observation\":" + JsonString(decision.deviceLossObservation) +
            L",\"cpu_capsule_exact\":" + (decision.cpuCapsuleExact ? L"true" : L"false") +
            L",\"gpu_attempted\":" + (decision.gpuAttempted ? L"true" : L"false") +
            L",\"gpu_exact\":" + (decision.gpuExact ? L"true" : L"false") +
            L",\"gpu_device_lost\":" + (decision.gpuDeviceLost ? L"true" : L"false") +
            L",\"controlled_fault\":" + (decision.controlledFault ? L"true" : L"false") +
            L",\"actual_device_removed\":" + (decision.actualDeviceRemoved ? L"true" : L"false") +
            L",\"gpu_quarantined\":" + (decision.gpuQuarantined ? L"true" : L"false") +
            L",\"automatic_backend_selection\":true" +
            L",\"production_authority\":true" +
            L",\"checkpoint_boundary\":\"after_completed_node_only\"" +
            L",\"cancellation_idempotent\":true" +
            L",\"runtime_native_codegen\":false" +
            L",\"runtime_shader_compilation\":false}";
    }
}
