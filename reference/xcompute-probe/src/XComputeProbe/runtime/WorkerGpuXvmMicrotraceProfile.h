#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmMicrotraceContractId =
        L"XVM_GPU_MICROTRACE_V1";
    inline constexpr wchar_t const* WorkerGpuXvmMicrotraceProfileId =
        L"gpu_microtrace_v1_identity_straight_line_u32_v1";
    inline constexpr uint32_t WorkerGpuXvmMicrotraceWordsPerOpValue = 8;
    inline constexpr uint32_t WorkerGpuXvmMicrotraceMaxOpsValue = 32;

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmMicrotraceV1Profile();
    std::wstring WorkerGpuXvmMicrotraceV1ProfileContractJson();
}