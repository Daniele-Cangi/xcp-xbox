#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

#include "WorkerCpuCapsuleModule.h"
#include "WorkerGpuXvmExecutionPlan.h"
#include "WorkerXvmSpmdRuntime.h"
#include "WorkerXvmTypes.h"
#include "../../shared/XComputeCpuCapsuleProvableWorldsV1Abi.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerCpuGpuConvergenceRequestSchemaVersion =
        L"cpu-gpu-evolved-backend-convergence-request-v1";
    inline constexpr wchar_t const* WorkerCpuGpuConvergencePlanSchemaVersion =
        L"cpu-gpu-evolved-backend-convergence-plan-v1";
    inline constexpr wchar_t const* WorkerCpuGpuConvergenceResultSchemaVersion =
        L"cpu-gpu-evolved-backend-convergence-result-v1";
    inline constexpr wchar_t const* WorkerCpuGpuConvergenceContractId =
        L"CPU_GPU_EVOLVED_BACKEND_CONVERGENCE_V1";
    inline constexpr wchar_t const* WorkerCpuGpuConvergenceProfileId =
        L"provable_worlds_cpu_capsule_v1";
    inline constexpr wchar_t const* WorkerCpuGpuConvergenceProfileContractSha256 =
        L"e0b6a59c07b7225135efb5fce4b0f72f18ddb903325fdbdf35b038415f366486";

    struct WorkerCpuGpuConvergencePlan
    {
        bool admitted = false;
        std::wstring schemaVersion;
        std::wstring contractId;
        std::wstring profileId;
        std::wstring profileContractSha256;
        std::wstring programSha256;
        std::wstring spmdExecutionPlanSha256;
        std::wstring gpuExecutionPlanSha256;
        std::wstring packageFullName;
        std::wstring packageVersion;
        std::wstring packageInstalledPath;
        std::wstring modulePath;
        std::wstring moduleSha256;
        std::wstring planSha256;
        uint64_t moduleBytes = 0;
        uint64_t aggregateFuel = 0;
        uint32_t abiVersion = 0;
        uint32_t laneCount = 0;
        uint32_t fieldBytes = 0;
        uint32_t maximumChunkLanes = 0;
    };

    struct WorkerCpuGpuConvergenceResult
    {
        WorkerCpuGpuConvergencePlan plan;
        std::wstring loadedModulePath;
        double referenceElapsedMs = 0.0;
        double moduleLoadElapsedMs = 0.0;
        double coldElapsedMs = 0.0;
        double warmElapsedMs = 0.0;
        uint64_t chunkCallCount = 0;
        std::vector<uint8_t> verifiedField;
        bool moduleWasLoadedBefore = false;
        bool canceled = false;
    };

    struct WorkerCpuGpuConvergenceExactTileResult
    {
        std::wstring packageFullName;
        std::wstring packageVersion;
        std::wstring modulePath;
        std::wstring moduleSha256;
        double moduleLoadElapsedMs = 0.0;
        double executeElapsedMs = 0.0;
        uint64_t sourceInstructions = 0;
        bool moduleWasLoadedBefore = false;
    };

    WorkerCpuGpuConvergencePlan WorkerBuildCpuGpuConvergencePlan(
        winrt::Windows::Data::Json::JsonObject const& node,
        WorkerXvmProgram const& program,
        WorkerXvmSpmdExecutionPlan const& spmdPlan,
        WorkerGpuXvmExecutionPlan const& gpuPlan,
        std::wstring const& programSha256);

    bool WorkerCpuGpuConvergencePlansEqual(
        WorkerCpuGpuConvergencePlan const& expected,
        WorkerCpuGpuConvergencePlan const& actual);

    void WorkerValidateCpuGpuConvergencePlan(
        WorkerCpuGpuConvergencePlan const& admittedPlan,
        bool hasAdmittedPlan,
        WorkerCpuGpuConvergencePlan const& rebuiltPlan);

    std::wstring WorkerCpuGpuConvergencePlanJson(
        WorkerCpuGpuConvergencePlan const& plan);

    WorkerCpuGpuConvergenceResult WorkerRunCpuGpuConvergenceCpuCandidate(
        WorkerCpuGpuConvergencePlan const& admittedPlan,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::vector<uint8_t> const& canonicalField,
        double referenceElapsedMs,
        std::function<bool()> const& cancelRequested = {});

    WorkerCpuGpuConvergenceExactTileResult
    WorkerRunCpuGpuConvergenceExactTile(
        uint32_t startLane,
        uint32_t laneCount,
        uint32_t inputSeed,
        std::vector<uint8_t> const& canonicalTile);

    std::wstring WorkerCpuGpuConvergenceResultJson(
        WorkerCpuGpuConvergenceResult const& result,
        bool gpuExactMatch);

    void WorkerQuarantineCpuGpuConvergenceCandidate() noexcept;
    bool WorkerIsCpuGpuConvergenceCandidateQuarantined() noexcept;
}
