#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphExpansion.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphValidation.h"
#include "WorkerXvmRuntime.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerGraphLegacyExecutionPlanSchemaVersion =
        L"worker-on-device-graph-execution-plan-v3";
    inline constexpr wchar_t const* WorkerGraphPreviousExecutionPlanSchemaVersion =
        L"worker-on-device-graph-execution-plan-v4";
    inline constexpr wchar_t const* WorkerGraphBackendConvergenceExecutionPlanSchemaVersion =
        L"worker-on-device-graph-execution-plan-v6";
    inline constexpr wchar_t const* WorkerGraphExecutionPlanSchemaVersion =
        L"worker-on-device-graph-execution-plan-v7";

    struct WorkerGraphAdmittedEdge
    {
        uint32_t sourceNodeIndex = 0;
        std::wstring fromNodeId;
        std::wstring role;
        std::wstring edgeType;
        std::wstring valueType;
        std::wstring expectedArtifactId;
    };

    struct WorkerGraphAdmittedNode
    {
        winrt::Windows::Data::Json::JsonObject node{ nullptr };
        std::wstring nodeId;
        std::wstring command;
        std::wstring resultArtifactId;
        std::wstring inputBindingMode;
        std::wstring xvmStaticFuelProofJson = L"null";
        std::wstring xvmGpuExecutionPlanJson = L"null";
        WorkerGpuXvmExecutionPlan xvmGpuExecutionPlan;
        std::wstring xvmSpmdExecutionPlanJson = L"null";
        WorkerXvmSpmdExecutionPlan xvmSpmdExecutionPlan;
        std::wstring xvmCpuExecutionPlanJson = L"null";
        WorkerXvmCpuExecutionPlan xvmCpuExecutionPlan;
        std::wstring xvmCpuCapsulePlanJson = L"null";
        WorkerXvmCpuCapsulePlan xvmCpuCapsulePlan;
        std::wstring xvmBackendConvergencePlanJson = L"null";
        WorkerCpuGpuConvergencePlan xvmBackendConvergencePlan;
        std::wstring xvmProductionModePlanJson = L"null";
        WorkerVerifiedProductionModePlan xvmProductionModePlan;
        double xvmCpuPlanBuildElapsedMs = 0.0;
        std::vector<WorkerGraphAdmittedEdge> inputEdges;
    };

    struct WorkerGraphExecutionPlan
    {
        winrt::Windows::Data::Json::JsonObject graph{ nullptr };
        std::wstring graphId;
        std::wstring graphSchemaVersion;
        std::vector<WorkerGraphAdmittedNode> nodes;
        WorkerGraphResourceLedger resourceLedger;
        WorkerGraphBoundedLoopValidation boundedLoop;
        WorkerGraphStaticNeighborReadValidation staticNeighborRead;
        WorkerGraphExpansionApplication expansion;
        uint64_t edgeCount = 0;
    };

    WorkerGraphExecutionPlan WorkerGraphAdmitExecutionPlan(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& generatedGraphId,
        WorkerXvmProgramResolver const& xvmResolver,
        WorkerXvmProgramTextReader const& xvmTextReader,
        WorkerXvmSnapshotAuthorityProvider const& xvmSnapshotAuthorityLoader);

    std::wstring WorkerGraphExecutionPlanJson(WorkerGraphExecutionPlan const& plan);
}
