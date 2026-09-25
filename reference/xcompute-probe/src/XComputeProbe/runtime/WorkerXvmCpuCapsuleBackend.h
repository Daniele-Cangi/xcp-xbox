#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "WorkerCpuCapsuleModule.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmCpuExecutionPlan.h"
#include "WorkerXvmInterpreter.h"
#include "WorkerXvmTypes.h"
#include "../../shared/XComputeCpuCapsuleHotKernelV1Abi.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerXvmCpuCapsuleBackendValue =
        L"cpu_packaged_capsule_hot_kernel_differential";
    inline constexpr wchar_t const* WorkerXvmCpuCapsulePlanSchemaVersion =
        L"xvm-cpu-packaged-capsule-plan-v1";
    inline constexpr wchar_t const* WorkerXvmCpuCapsuleProfileId =
        L"xvm_cpu_hot_kernel_mix65536_trap_v1";
    inline constexpr wchar_t const* WorkerXvmCpuCapsuleProfileContractSha256 =
        L"fc911fe4581e08112bb47a7cd8852185fc4d4d22f716a1516acd17ee48eead11";

    struct WorkerXvmCpuCapsulePlan
    {
        bool admitted = false;
        std::wstring schemaVersion;
        std::wstring profileId;
        std::wstring profileContractSha256;
        std::wstring programSha256;
        std::wstring executionPlanSha256;
        std::wstring packageFullName;
        std::wstring packageVersion;
        std::wstring packageInstalledPath;
        std::wstring modulePath;
        std::wstring moduleSha256;
        uint64_t moduleBytes = 0;
        uint32_t abiVersion = 0;
        uint32_t loopIterations = 0;
        uint32_t maximumChunkIterations = 0;
    };

    struct WorkerXvmCpuCapsuleDifferentialResult
    {
        WorkerXvmRunResult result;
        WorkerXvmMachineState checkpointState;
        WorkerXvmMachineState canceledState;
        WorkerXvmCpuCapsulePlan plan;
        std::wstring loadedModulePath;
        double referenceElapsedMs = 0.0;
        double moduleLoadElapsedMs = 0.0;
        double coldElapsedMs = 0.0;
        double warmElapsedMs = 0.0;
        double abiCallOverheadNs = 0.0;
        double checkpointElapsedMs = 0.0;
        uint64_t chunkCallCount = 0;
        bool moduleWasLoadedBefore = false;
        bool checkpointVerified = false;
        bool canceled = false;
    };

    void WorkerQuarantineXvmCpuCapsule() noexcept;
    bool WorkerIsXvmCpuCapsuleQuarantined() noexcept;
    std::wstring WorkerXvmCpuCapsuleQuarantineDiagnosticJson(std::wstring const& protocolVersion);

    WorkerXvmCpuCapsulePlan WorkerBuildXvmCpuCapsulePlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& executionPlan,
        std::wstring const& programSha256);

    bool WorkerXvmCpuCapsulePlansEqual(
        WorkerXvmCpuCapsulePlan const& expected,
        WorkerXvmCpuCapsulePlan const& actual);

    std::wstring WorkerXvmCpuCapsulePlanJson(
        WorkerXvmCpuCapsulePlan const& plan);

    WorkerXvmCpuCapsuleDifferentialResult WorkerRunXvmCpuCapsuleDifferential(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& executionPlan,
        WorkerXvmCpuCapsulePlan const& admittedPlan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState const& initialState,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested = {});

    std::wstring WorkerXvmCpuCapsuleExecutionJson(
        WorkerXvmCpuCapsuleDifferentialResult const& result,
        bool deoptimizedToInterpreter);
}
