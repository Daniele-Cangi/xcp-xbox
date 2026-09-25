#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmSpmdPhaseBContractId =
        L"GPU_XVM_SPMD_MANY_LANE_V1_PHASE_B_GPU_DIFFERENTIAL";
    inline constexpr wchar_t const* WorkerGpuXvmSpmdPhaseBProfileId =
        L"spmd_many_lane_v1_phase_b_4x3_u32_v1";

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmSpmdPhaseBProfile();
    bool WorkerGpuXvmSpmdPhaseBProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);
    std::wstring WorkerGpuXvmSpmdPhaseBProfileContractJson();
}