#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerGraphPublishedArtifact
    {
        std::wstring artifactId;
        std::wstring artifactKind;
        std::wstring sha256;
        std::wstring blobHandle;
        std::wstring manifestPath;
        uint64_t bytes = 0;
        uint64_t workspaceBytesBefore = 0;
        uint64_t workspaceBytesAfter = 0;
        uint64_t workspaceBudgetBytes = 0;
        uint64_t rollbackHeadroomBytes = 0;
        uint64_t storageAvailableBytes = 0;
        bool storageAvailableKnown = false;
        bool deduplicated = false;
        double publishMs = 0.0;
    };

    struct WorkerGraphPublishInput
    {
        winrt::Windows::Data::Json::JsonObject request{ nullptr };
        winrt::Windows::Data::Json::JsonObject graph{ nullptr };
        std::wstring protocolVersion;
        std::wstring graphSchemaVersion;
        std::wstring graphId;
        std::wstring executionPlanSchemaVersion;
        std::wstring executionPlanJson = L"{}";
        std::wstring executionPlanSha256;
        std::wstring nodesJson;
        std::wstring canonicalNodesJson;
        std::wstring graphResultSha256;
        std::wstring graphResultCanonicalSha256;
        std::wstring graphSpecSha256;
        std::wstring nodeLineageSha256;
        std::wstring canonicalNodeLineageSha256;
        std::wstring resourceLedgerJson = L"{}";
        std::wstring boundedLoopProfileJson = L"{}";
        std::wstring staticNeighborReadProfileJson = L"{}";
        std::wstring graphExpansionProfileJson = L"{}";
        std::wstring checkpointLogJson = L"[]";
        std::wstring checkpointArtifactsJson = L"[]";
        std::wstring checkpointChainSha256;
        std::wstring checkpointReplayJson = L"{}";
        std::wstring verdict;
        uint32_t nodeCount = 0;
        uint64_t edgeCount = 0;
        uint64_t passedNodeCount = 0;
        uint64_t failedNodeCount = 0;
        uint64_t totalLogicalBytes = 0;
        uint64_t preAdmittedNodeCount = 0;
        uint64_t preAdmittedEdgeCount = 0;
        uint64_t xvmNodeCount = 0;
        uint64_t boundedLoopIterationCount = 0;
        uint64_t boundedLoopMaxIterations = 0;
        uint64_t boundedLoopFuelBudget = 0;
        uint64_t boundedLoopFuelConsumed = 0;
        uint64_t staticNeighborReadNeighborCount = 0;
        uint64_t graphExpansionPreNodeCount = 0;
        uint64_t graphExpansionExpandedNodeCount = 0;
        uint64_t graphExpansionExpandedEdgeCount = 0;
        uint64_t checkpointCount = 0;
        bool fullGraphPreAdmission = false;
        bool boundedLoopProfileAdmitted = false;
        bool staticNeighborReadProfileAdmitted = false;
        bool graphExpansionProfileAdmitted = false;
        bool checkpointReplayVerified = false;
    };

    struct WorkerGraphPublishResult
    {
        WorkerGraphPublishedArtifact graphResult;
        WorkerGraphPublishedArtifact evidenceBundle;
        bool evidenceBundlePublished = false;
        std::wstring evidenceBundleSealSha256;
        std::wstring evidenceResponseFields;
    };

    using WorkerGraphArtifactPublisher = std::function<WorkerGraphPublishedArtifact(
        winrt::Windows::Data::Json::JsonObject const& publishRequest,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields)>;

    using WorkerGraphSha256Text = std::function<std::wstring(std::string const& text)>;

    WorkerGraphPublishResult WorkerGraphPublishArtifacts(
        WorkerGraphPublishInput const& input,
        WorkerGraphArtifactPublisher const& publisher,
        WorkerGraphSha256Text const& sha256Text);

    std::wstring WorkerGraphSubmitGraphResponse(
        WorkerGraphPublishInput const& input,
        WorkerGraphPublishResult const& publishResult,
        double elapsedMs);
}
