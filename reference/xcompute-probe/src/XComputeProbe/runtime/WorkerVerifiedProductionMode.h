#pragma once

#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerCpuGpuConvergenceBackend.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerVerifiedProductionModeRequestSchemaVersion =
        L"gpu-xvm-verified-production-mode-request-v1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModePlanSchemaVersion =
        L"gpu-xvm-verified-production-mode-plan-v1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModeResultSchemaVersion =
        L"gpu-xvm-verified-production-mode-result-v1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModeContractId =
        L"GPU_XVM_VERIFIED_PRODUCTION_MODE_V1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModePolicyId =
        L"provable_worlds_verified_production_v1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModePolicySha256 =
        L"4c2e30a247459e5a5b2edc574511569a8e292ba383fb30ada7846bccaabe0269";
    inline constexpr wchar_t const* WorkerVerifiedProductionModeSelectionPolicy =
        L"verified_gpu_then_cpu_capsule_then_cpu_reference_v1";
    inline constexpr wchar_t const* WorkerVerifiedProductionModeNoFault = L"none";
    inline constexpr wchar_t const* WorkerVerifiedProductionModeControlledDeviceLoss =
        L"controlled_gpu_device_loss_before_dispatch";

    struct WorkerVerifiedProductionModePlan
    {
        bool admitted = false;
        bool gpuEligibleAtAdmission = false;
        std::wstring schemaVersion;
        std::wstring contractId;
        std::wstring policyId;
        std::wstring policySha256;
        std::wstring selectionPolicy;
        std::wstring validationFault;
        std::wstring convergencePlanSha256;
        std::wstring gpuExecutionPlanSha256;
        std::wstring capsuleModuleSha256;
        std::wstring planSha256;
    };

    struct WorkerVerifiedProductionModeDecision
    {
        WorkerVerifiedProductionModePlan plan;
        std::wstring selectedBackend;
        std::wstring assuranceClass;
        std::wstring verificationMethod;
        std::wstring fallbackReason;
        std::wstring deviceLossObservation;
        bool cpuCapsuleExact = false;
        bool gpuAttempted = false;
        bool gpuExact = false;
        bool gpuDeviceLost = false;
        bool controlledFault = false;
        bool actualDeviceRemoved = false;
        bool gpuQuarantined = false;
        bool automaticSelection = false;
        bool productionAuthority = false;
    };

    WorkerVerifiedProductionModePlan WorkerBuildVerifiedProductionModePlan(
        winrt::Windows::Data::Json::JsonObject const& node,
        WorkerCpuGpuConvergencePlan const& convergencePlan);

    bool WorkerVerifiedProductionModePlansEqual(
        WorkerVerifiedProductionModePlan const& expected,
        WorkerVerifiedProductionModePlan const& actual);

    void WorkerValidateVerifiedProductionModePlan(
        WorkerVerifiedProductionModePlan const& admittedPlan,
        bool hasAdmittedPlan,
        WorkerVerifiedProductionModePlan const& rebuiltPlan);

    std::wstring WorkerVerifiedProductionModePlanJson(
        WorkerVerifiedProductionModePlan const& plan);

    bool WorkerVerifiedProductionModeShouldInjectGpuDeviceLoss(
        WorkerVerifiedProductionModePlan const& plan) noexcept;

    void WorkerVerifiedProductionModeQuarantineGpu() noexcept;
    bool WorkerVerifiedProductionModeGpuQuarantined() noexcept;

    WorkerVerifiedProductionModeDecision WorkerSelectVerifiedProductionModeAuthority(
        WorkerVerifiedProductionModePlan const& plan,
        bool cpuCapsuleExact,
        bool gpuAttempted,
        bool gpuExact,
        bool gpuDeviceLost,
        bool controlledFault,
        bool actualDeviceRemoved);

    std::wstring WorkerVerifiedProductionModeResultJson(
        WorkerVerifiedProductionModeDecision const& decision);
}
