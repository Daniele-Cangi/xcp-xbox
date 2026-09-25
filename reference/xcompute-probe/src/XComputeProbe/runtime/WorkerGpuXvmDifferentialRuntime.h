#pragma once

#include <array>
#include <functional>
#include <string>

#include "WorkerCpuGpuConvergenceBackend.h"
#include "WorkerGpuXvmExecutor.h"
#include "WorkerGraphPublishing.h"
#include "WorkerVerifiedProductionMode.h"

namespace XComputeProbe
{
    struct WorkerGpuXvmDifferentialInput
    {
        WorkerGpuXvmProfileDescriptor const* profile = nullptr;
        WorkerXvmProgram const* program = nullptr;
        WorkerGraphNodeResourceLimits const* limits = nullptr;
        WorkerGpuXvmExecutionPlan const* executionPlan = nullptr;
        WorkerXvmRunResult const* cpuRun = nullptr;
        WorkerCpuGpuConvergenceResult const* convergence = nullptr;
        WorkerVerifiedProductionModePlan productionPlan;
        std::array<uint32_t, WorkerXvmInputWordCountValue> inputs{};
        std::function<bool()> cancelRequested;
        std::wstring backend;
        std::wstring cpuOutputHex;
        std::wstring cpuFieldSha256;
        std::wstring provableWorldsFieldArtifactId;
        std::wstring provableWorldsVerificationArtifactId;
        std::wstring provableWorldsAssuranceClass;
        double cpuReferenceElapsedMs = 0.0;
        bool gpuSpmdDifferential = false;
        bool gpuMicrotraceDifferential = false;
        bool gpuMicrotraceOptimized = false;
        bool provableWorlds = false;
    };

    struct WorkerGpuXvmDifferentialResult
    {
        std::wstring gpuDifferentialJson =
            L"{\"enabled\":false,\"backend\":\"cpu_reference\"}";
        std::wstring microtraceExecutionJson =
            L"{\"enabled\":false,\"schema_version\":\"xvm-gpu-microtrace-execution-result-v1\"}";
        std::wstring provableWorldsJson =
            L"{\"enabled\":false,\"schema_version\":\"provable-worlds-result-v1\"}";
        std::wstring productionModeJson =
            L"{\"enabled\":false,\"schema_version\":\"gpu-xvm-verified-production-mode-result-v1\"}";
        bool gpuDifferential = false;
        bool gpuAttempted = false;
        bool gpuExact = false;
        bool gpuDeviceLost = false;
    };

    WorkerGpuXvmDifferentialResult WorkerRunGpuXvmDifferential(
        WorkerGpuXvmDifferentialInput const& input,
        WorkerGraphArtifactPublisher const& publisher,
        std::function<std::wstring(std::string const&)> const& sha256Text);
}
