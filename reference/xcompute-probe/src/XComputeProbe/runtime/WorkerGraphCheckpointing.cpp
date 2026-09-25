#include "pch.h"

#include "WorkerGraphCheckpointing.h"

#include "WorkerGraphRuntime.h"
#include "WorkerGraphValidation.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        std::string WideToUtf8(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        void InsertString(JsonObject& object, wchar_t const* name, std::wstring const& value)
        {
            object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
        }

        std::wstring BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring CheckpointArtifactBase(
            JsonObject const& request,
            JsonObject const& graph,
            std::wstring const& graphId)
        {
            auto base = WorkerGraphOptionalString(
                graph,
                L"checkpoint_artifact_id",
                WorkerGraphOptionalString(request, L"checkpoint_artifact_id"));
            if (base.empty())
            {
                base = graphId + L"-checkpoint";
            }
            if (base.size() > 54)
            {
                base.resize(54);
            }
            return base;
        }

        std::wstring WorkerGraphCheckpointArtifactId(
            std::wstring const& artifactBase,
            uint64_t checkpointOrdinal)
        {
            return artifactBase + L"-" + std::to_wstring(checkpointOrdinal);
        }

        std::wstring WorkerGraphCheckpointEntryJson(
            std::wstring const& graphId,
            uint64_t checkpointOrdinal,
            WorkerGraphCheckpointRecordInput const& input,
            std::wstring const& nodeLineageSha256,
            std::wstring const& canonicalNodeLineageSha256,
            std::wstring const& previousCheckpointChainSha256)
        {
            return std::wstring(L"{\"schema_version\":\"worker-graph-checkpoint-0.1\"") +
                L",\"graph_id\":" + JsonString(graphId) +
                L",\"checkpoint_ordinal\":" + std::to_wstring(checkpointOrdinal) +
                L",\"completed_node_count\":" + std::to_wstring(input.completedNodeCount) +
                L",\"resume_cursor_node_index\":" + std::to_wstring(input.completedNodeCount) +
                L",\"last_node_id\":" + JsonString(input.lastNodeId) +
                L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
                L",\"passed_node_count\":" + std::to_wstring(input.passedNodeCount) +
                L",\"failed_node_count\":" + std::to_wstring(input.failedNodeCount) +
                L",\"total_logical_bytes_read\":" + std::to_wstring(input.totalLogicalBytes) +
                L",\"node_lineage_sha256\":" + JsonString(nodeLineageSha256) +
                L",\"canonical_node_lineage_sha256\":" + JsonString(canonicalNodeLineageSha256) +
                L",\"previous_checkpoint_chain_sha256\":" + JsonString(previousCheckpointChainSha256) +
                L",\"resource_ledger\":" + input.resourceLedgerJson +
                L",\"recovery_ready\":true}";
        }

        std::wstring WorkerGraphCheckpointLogPayloadJson(
            std::wstring const& protocolVersion,
            std::wstring const& graphId,
            uint64_t checkpointCount,
            std::wstring const& checkpointChainSha256,
            WorkerGraphCheckpointRecordInput const& input,
            std::wstring const& nodeLineageSha256,
            std::wstring const& canonicalNodeLineageSha256,
            std::wstring const& checkpointLogJson)
        {
            return std::wstring(L"{\"schema_version\":\"worker-graph-checkpoint-log-0.1\"") +
                L",\"protocol_version\":" + JsonString(protocolVersion) +
                L",\"graph_id\":" + JsonString(graphId) +
                L",\"checkpoint_count\":" + std::to_wstring(checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(checkpointChainSha256) +
                L",\"latest_node_id\":" + JsonString(input.lastNodeId) +
                L",\"node_lineage_sha256\":" + JsonString(nodeLineageSha256) +
                L",\"canonical_node_lineage_sha256\":" + JsonString(canonicalNodeLineageSha256) +
                L",\"checkpoints\":" + checkpointLogJson +
                L",\"completed_nodes\":" + input.completedNodesJson +
                L",\"canonical_completed_nodes\":" + input.canonicalCompletedNodesJson +
                L",\"resumable_state\":true}";
        }

        std::wstring WorkerGraphCheckpointManifestFields(
            std::wstring const& graphId,
            uint64_t checkpointCount,
            std::wstring const& checkpointChainSha256,
            std::wstring const& canonicalNodeLineageSha256)
        {
            return std::wstring(L",\"job_output_schema\":\"worker-graph-checkpoint-log-0.1\"") +
                L",\"graph_id\":" + JsonString(graphId) +
                L",\"checkpoint_count\":" + std::to_wstring(checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(checkpointChainSha256) +
                L",\"canonical_node_lineage_sha256\":" + JsonString(canonicalNodeLineageSha256);
        }

        std::wstring WorkerGraphCheckpointArtifactSummaryJson(
            uint64_t checkpointOrdinal,
            WorkerGraphPublishedArtifact const& artifact)
        {
            return L"{\"checkpoint_ordinal\":" + std::to_wstring(checkpointOrdinal) +
                L",\"artifact_id\":" + JsonString(artifact.artifactId) +
                L",\"artifact_kind\":" + JsonString(artifact.artifactKind) +
                L",\"artifact_sha256\":" + JsonString(artifact.sha256) +
                L",\"artifact_bytes\":" + std::to_wstring(artifact.bytes) +
                L"}";
        }

        std::wstring WorkerGraphCheckpointReplayJson(
            uint64_t checkpointCount,
            bool verified,
            std::wstring const& checkpointChainSha256,
            std::wstring const& checkpointCanonicalSha256,
            std::wstring const& graphResultCanonicalSha256,
            std::wstring const& checkpointNodeLineageSha256,
            std::wstring const& graphResultSha256)
        {
            return std::wstring(L"{\"schema_version\":\"worker-graph-checkpoint-replay-0.1\"") +
                L",\"checkpoint_count\":" + std::to_wstring(checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(checkpointChainSha256) +
                L",\"checkpoint_canonical_node_lineage_sha256\":" + JsonString(checkpointCanonicalSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(graphResultCanonicalSha256) +
                L",\"checkpoint_node_lineage_sha256\":" + JsonString(checkpointNodeLineageSha256) +
                L",\"graph_result_sha256\":" + JsonString(graphResultSha256) +
                L",\"verified\":" + BoolJson(verified) +
                L"}";
        }
    }

    WorkerGraphCheckpointJournal::WorkerGraphCheckpointJournal(
        JsonObject const& request,
        JsonObject const& graph,
        std::wstring const& graphId,
        std::wstring const& protocolVersion,
        WorkerGraphArtifactPublisher const& artifactPublisher,
        WorkerGraphSha256Text const& sha256Text,
        WorkerGraphCheckpointObserver const& checkpointObserver) :
        artifactBase_(CheckpointArtifactBase(request, graph, graphId)),
        graphId_(graphId),
        protocolVersion_(protocolVersion),
        artifactPublisher_(artifactPublisher),
        sha256Text_(sha256Text),
        checkpointObserver_(checkpointObserver)
    {
    }

    void WorkerGraphCheckpointJournal::Publish(WorkerGraphCheckpointRecordInput const& input)
    {
        latestNodeLineageSha256_ = sha256Text_(WideToUtf8(input.completedNodesJson));
        latestCanonicalNodeLineageSha256_ = sha256Text_(WideToUtf8(input.canonicalCompletedNodesJson));

        auto const previousCheckpointChainSha256 = chainSha256_;
        auto const checkpointSeed = previousCheckpointChainSha256.empty()
            ? std::wstring(L"worker-graph-checkpoint-chain-0.1")
            : previousCheckpointChainSha256;
        auto const checkpointOrdinal = static_cast<uint64_t>(entries_.size()) + 1;
        auto const checkpointEntry = WorkerGraphCheckpointEntryJson(
            graphId_,
            checkpointOrdinal,
            input,
            latestNodeLineageSha256_,
            latestCanonicalNodeLineageSha256_,
            previousCheckpointChainSha256);
        chainSha256_ = sha256Text_(WideToUtf8(checkpointSeed + L"\n" + checkpointEntry));
        entries_.push_back(checkpointEntry);

        auto const checkpointLogJson = WorkerGraphJsonArray(entries_);
        auto const checkpointPayload = WorkerGraphCheckpointLogPayloadJson(
            protocolVersion_,
            graphId_,
            checkpointOrdinal,
            chainSha256_,
            input,
            latestNodeLineageSha256_,
            latestCanonicalNodeLineageSha256_,
            checkpointLogJson);
        auto const checkpointArtifactId = WorkerGraphCheckpointArtifactId(artifactBase_, checkpointOrdinal);

        JsonObject checkpointPublishRequest;
        InsertString(checkpointPublishRequest, L"artifact_id", checkpointArtifactId);
        InsertString(checkpointPublishRequest, L"artifact_kind", L"on-device-graph-checkpoint-log");
        auto const checkpointPublished = artifactPublisher_(
            checkpointPublishRequest,
            checkpointArtifactId,
            L"on-device-graph-checkpoint-log",
            WideToUtf8(checkpointPayload),
            WorkerGraphCheckpointManifestFields(
                graphId_,
                checkpointOrdinal,
                chainSha256_,
                latestCanonicalNodeLineageSha256_));
        artifacts_.push_back(WorkerGraphCheckpointArtifactSummaryJson(checkpointOrdinal, checkpointPublished));

        if (checkpointObserver_)
        {
            checkpointObserver_(checkpointOrdinal, checkpointPublished.artifactId);
        }
    }

    WorkerGraphCheckpointResult WorkerGraphCheckpointJournal::Finalize(
        uint64_t expectedNodeCount,
        std::wstring const& graphResultCanonicalSha256,
        std::wstring const& graphResultSha256) const
    {
        WorkerGraphCheckpointResult result;
        result.count = static_cast<uint64_t>(entries_.size());
        result.logJson = WorkerGraphJsonArray(entries_);
        result.artifactsJson = WorkerGraphJsonArray(artifacts_);
        result.chainSha256 = chainSha256_;
        result.replayVerified =
            result.count == expectedNodeCount &&
            latestCanonicalNodeLineageSha256_ == graphResultCanonicalSha256 &&
            latestNodeLineageSha256_ == graphResultSha256 &&
            !chainSha256_.empty();
        result.replayJson = WorkerGraphCheckpointReplayJson(
            result.count,
            result.replayVerified,
            chainSha256_,
            latestCanonicalNodeLineageSha256_,
            graphResultCanonicalSha256,
            latestNodeLineageSha256_,
            graphResultSha256);

        if (!result.replayVerified)
        {
            throw WorkerGraphValidationError(
                "submit_graph.checkpoint_replay_failed",
                "graph checkpoint replay did not match the final graph result");
        }
        return result;
    }
}
