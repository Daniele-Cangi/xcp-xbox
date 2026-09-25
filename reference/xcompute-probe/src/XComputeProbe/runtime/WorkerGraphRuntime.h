#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace XComputeProbe
{
    struct WorkerGraphNodeRecord
    {
        std::wstring nodeId;
        std::wstring resultArtifactId;
        std::wstring resultSha256;
        std::wstring logicalSha256;
        std::wstring computeMix64;
        std::wstring kernelId;
        std::wstring controlToken;
        uint64_t fuelConsumed = 0;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
        bool ok = false;
    };

    struct WorkerGraphNodeResultSummary
    {
        std::wstring nodeId;
        std::wstring command;
        std::wstring kernelId;
        std::wstring computeKind;
        std::wstring manifestArtifactId;
        std::wstring resultArtifactId;
        std::wstring resultSha256;
        std::wstring logicalSha256;
        std::wstring computeMix64;
        std::wstring inputBindingMode;
        std::wstring boundInputSha256;
        std::wstring boundInputMix64;
        std::wstring inputEdgesJson;
        std::wstring controlToken;
        std::wstring resourceUsageJson = L"{}";
        std::wstring resultEnvelopeJson = L"{}";
        std::wstring functionalCanonicalJson = L"{}";
        std::wstring executionDetailsJson = L"{}";
        uint64_t logicalBytesRead = 0;
        uint64_t inputEdgeCount = 0;
        uint64_t fuelConsumed = 0;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
        bool ok = false;
        bool boundInputApplied = false;
        bool verified = false;
    };

    uint32_t WorkerMaxTaskPlanSteps();
    uint32_t WorkerMaxOnDeviceGraphNodes();
    uint32_t WorkerMaxOnDeviceGraphEdges();
    uint32_t WorkerGraphMaxBoundedLoopIterations();
    uint32_t WorkerGraphMaxStaticNeighborReadEdges();
    uint32_t WorkerGraphMaxExpansionNodes();

    wchar_t const* WorkerGraphEvidenceBundleSchemaVersion();
    wchar_t const* WorkerGraphEvidenceSealSchemaVersion();

    std::wstring WorkerGraphJsonArray(std::vector<std::wstring> const& jsonItems);
    std::wstring WorkerGraphInputEdgeJson(
        WorkerGraphNodeRecord const& source,
        std::wstring const& role,
        std::wstring const& edgeType,
        std::wstring const& valueType);
    std::wstring WorkerGraphDeterministicInputEdgeJson(
        WorkerGraphNodeRecord const& source,
        std::wstring const& role,
        std::wstring const& edgeType,
        std::wstring const& valueType);

    std::vector<std::wstring> WorkerGraphReduceKernelIds();
    std::vector<std::wstring> WorkerGraphControlKernelIds();
    std::vector<std::wstring> WorkerGraphStaticMultipassProfileIds();
    std::vector<std::wstring> WorkerGraphBoundedLoopProfileIds();
    std::vector<std::wstring> WorkerGraphStaticNeighborReadProfileIds();
    std::vector<std::wstring> WorkerGraphExpansionProfileIds();
    std::vector<std::wstring> WorkerGraphInputBindingModes();
    std::vector<std::wstring> WorkerSupportedCommandIds();
}
