#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGpuXvmProvableWorldsContractId =
        L"PROVABLE_WORLDS_V1";
    inline constexpr wchar_t const* WorkerGpuXvmProvableWorldsProfileId =
        L"provable_worlds_v1_1024x1024_u32";

    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmProvableWorldsProfile();
    bool WorkerGpuXvmProvableWorldsProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);
    std::wstring WorkerGpuXvmProvableWorldsProfileContractJson();
}
