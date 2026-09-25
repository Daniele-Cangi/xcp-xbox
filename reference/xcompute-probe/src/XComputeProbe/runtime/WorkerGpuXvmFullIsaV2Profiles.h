#pragma once

#include "WorkerGpuXvmProfileContract.h"

namespace XComputeProbe
{
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2FullCallReturnProfile();
    WorkerGpuXvmProfileDescriptor const& WorkerGpuXvmFullIsaV2LegacyBranchProfile();

    WorkerGpuXvmProfileDescriptor const* WorkerFindGpuXvmFullIsaV2Profile(
        std::wstring const& profileId);

    bool WorkerGpuXvmFullIsaV2ProfileMatches(
        WorkerGpuXvmProfileDescriptor const& profile,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits);
}
