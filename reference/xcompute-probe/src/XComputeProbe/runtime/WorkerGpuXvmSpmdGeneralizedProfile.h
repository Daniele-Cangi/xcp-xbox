#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmSpmdGeneralizedContractId =
        L"GPU_XVM_SPMD_MANY_LANE_V1_GENERALIZED_TOPOLOGY";
    inline constexpr wchar_t const* WorkerGpuXvmSpmdGeneralizedProfileId =
        L"spmd_many_lane_v1_generalized_8x8_u32_v1";

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmSpmdGeneralizedProfile();
    bool WorkerGpuXvmSpmdGeneralizedProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);
    std::wstring WorkerGpuXvmSpmdGeneralizedProfileContractJson();
}