#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmMicrotracePhaseBContractId =
        L"XVM_GPU_MICROTRACE_V1_PHASE_B_OPTIMIZATION";
    inline constexpr wchar_t const* WorkerGpuXvmMicrotracePhaseBProfileId =
        L"gpu_microtrace_v1_phase_b_optimized_straight_line_u32_v1";
    inline constexpr uint32_t WorkerGpuXvmMicrotracePhaseBWordsPerOpValue = 16;
    inline constexpr uint32_t WorkerGpuXvmMicrotracePhaseBMaxOpsValue = 32;

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotracePhaseBV1Profile();
    std::wstring WorkerGpuXvmMicrotracePhaseBV1ProfileContractJson();
}
