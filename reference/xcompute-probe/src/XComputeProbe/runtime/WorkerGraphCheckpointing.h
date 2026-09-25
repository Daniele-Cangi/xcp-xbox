#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphPublishing.h"

namespace XComputeProbe
{
    using WorkerGraphCheckpointObserver = std::function<void(
        uint64_t checkpointOrdinal,
        std::wstring const& artifactId)>;

    struct WorkerGraphCheckpointRecordInput
    {
        uint64_t completedNodeCount = 0;
        uint64_t edgeCount = 0;
        uint64_t passedNodeCount = 0;
        uint64_t failedNodeCount = 0;
        uint64_t totalLogicalBytes = 0;
        std::wstring lastNodeId;
        std::wstring resourceLedgerJson = L"{}";
        std::wstring completedNodesJson = L"[]";
        std::wstring canonicalCompletedNodesJson = L"[]";
    };

    struct WorkerGraphCheckpointResult
    {
        std::wstring logJson = L"[]";
        std::wstring artifactsJson = L"[]";
        std::wstring chainSha256;
        std::wstring replayJson = L"{}";
        uint64_t count = 0;
        bool replayVerified = false;
    };

    class WorkerGraphCheckpointJournal
    {
    public:
        WorkerGraphCheckpointJournal(
            winrt::Windows::Data::Json::JsonObject const& request,
            winrt::Windows::Data::Json::JsonObject const& graph,
            std::wstring const& graphId,
            std::wstring const& protocolVersion,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphCheckpointObserver const& checkpointObserver = {});

        void Publish(WorkerGraphCheckpointRecordInput const& input);

        WorkerGraphCheckpointResult Finalize(
            uint64_t expectedNodeCount,
            std::wstring const& graphResultCanonicalSha256,
            std::wstring const& graphResultSha256) const;

    private:
        std::wstring artifactBase_;
        std::wstring graphId_;
        std::wstring protocolVersion_;
        WorkerGraphArtifactPublisher artifactPublisher_;
        WorkerGraphSha256Text sha256Text_;
        WorkerGraphCheckpointObserver checkpointObserver_;
        std::vector<std::wstring> entries_;
        std::vector<std::wstring> artifacts_;
        std::wstring chainSha256_;
        std::wstring latestNodeLineageSha256_;
        std::wstring latestCanonicalNodeLineageSha256_;
    };
}
