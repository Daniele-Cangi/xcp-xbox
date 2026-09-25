#include "pch.h"
#include "WorkerGraphPublishing.h"
#include "WorkerGraphRuntime.h"
#include "WorkerGraphValidation.h"
#include "WorkerResultEnvelope.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        std::string WideToUtf8Local(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring DoubleJsonLocal(double value, int precision)
        {
            if (!std::isfinite(value))
            {
                return L"null";
            }
            std::wostringstream out;
            out << std::fixed << std::setprecision(precision) << value;
            return out.str();
        }

        uint64_t OptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            return static_cast<uint64_t>(object.GetNamedNumber(name, static_cast<double>(fallback)));
        }

        void InsertString(JsonObject& object, wchar_t const* name, std::wstring const& value)
        {
            object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
        }

        void InsertNumber(JsonObject& object, wchar_t const* name, uint64_t value)
        {
            object.Insert(name, JsonValue::CreateNumberValue(static_cast<double>(value)));
        }

        std::wstring DefaultGraphArtifactId(std::wstring const& graphId, wchar_t const* suffix)
        {
            auto prefix = graphId;
            if (prefix.size() > 42)
            {
                prefix.resize(42);
            }
            return prefix + suffix;
        }

        std::wstring GraphClaimBoundaryJson(WorkerGraphPublishInput const& input)
        {
            return std::wstring(L"{\"arbitrary_code\":false") +
                L",\"arbitrary_code_legacy_meaning\":\"native_or_host_code\"" +
                L",\"arbitrary_native_or_host_code\":false" +
                L",\"admitted_xvm_bytecode\":true" +
                L",\"precompiled_gpu_spmd_graph_node\":true" +
                L",\"graph_checkpoint_recovery\":true" +
                L",\"graph_async_cancellation\":true" +
                L",\"bounded_static_loop\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
                L",\"bounded_loop\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
                L",\"bounded_loop_dynamic_expansion\":false" +
                L",\"static_neighbor_read\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
                L",\"neighbor_reads\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
                L",\"neighbor_read_dynamic_discovery\":false" +
                L",\"shell\":false" +
                L",\"runtime_shader_compilation\":false" +
                L",\"executable_upload\":false" +
                L",\"dynamic_graph_branching\":false" +
                L",\"graph_expansion\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
                L",\"validated_graph_expansion\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
                L",\"high_fidelity_physics\":false" +
                L",\"terabyte_scale\":false" +
                L",\"multi_device_cluster\":false" +
                L",\"full_12_1_tflops\":false}";
        }

        std::wstring GraphResultPayload(WorkerGraphPublishInput const& input)
        {
            return std::wstring(L"{\"schema_version\":\"worker-on-device-graph-result-artifact-0.1\"") +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"graph_id\":" + JsonString(input.graphId) +
                L",\"execution_plan_schema\":" + JsonString(input.executionPlanSchemaVersion) +
                L",\"execution_plan_sha256\":" + JsonString(input.executionPlanSha256) +
                L",\"full_graph_pre_admission\":" + std::wstring(input.fullGraphPreAdmission ? L"true" : L"false") +
                L",\"pre_admitted_node_count\":" + std::to_wstring(input.preAdmittedNodeCount) +
                L",\"pre_admitted_edge_count\":" + std::to_wstring(input.preAdmittedEdgeCount) +
                L",\"execution_plan\":" + input.executionPlanJson +
                L",\"runtime_mode\":\"bounded_on_device_graph_v1\"" +
                L",\"programmable_runtime_foundation\":true" +
                L",\"shared_result_envelope_schema\":" + JsonString(WorkerResultEnvelopeSchemaVersion()) +
                L",\"node_count\":" + std::to_wstring(input.nodeCount) +
                L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
                L",\"passed_node_count\":" + std::to_wstring(input.passedNodeCount) +
                L",\"failed_node_count\":" + std::to_wstring(input.failedNodeCount) +
                L",\"total_logical_bytes_read\":" + std::to_wstring(input.totalLogicalBytes) +
                L",\"graph_result_sha256\":" + JsonString(input.graphResultSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
                L",\"graph_result_canonical_schema\":\"worker-on-device-graph-canonical-result-0.1\"" +
                L",\"resource_ledger\":" + input.resourceLedgerJson +
                L",\"bounded_loop_profile_admitted\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
                L",\"bounded_loop_profile\":" + input.boundedLoopProfileJson +
                L",\"bounded_loop_iteration_count\":" + std::to_wstring(input.boundedLoopIterationCount) +
                L",\"bounded_loop_max_iterations\":" + std::to_wstring(input.boundedLoopMaxIterations) +
                L",\"bounded_loop_fuel_budget\":" + std::to_wstring(input.boundedLoopFuelBudget) +
                L",\"bounded_loop_fuel_consumed\":" + std::to_wstring(input.boundedLoopFuelConsumed) +
                L",\"static_neighbor_read_profile_admitted\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
                L",\"static_neighbor_read_profile\":" + input.staticNeighborReadProfileJson +
                L",\"static_neighbor_read_neighbor_count\":" + std::to_wstring(input.staticNeighborReadNeighborCount) +
                L",\"graph_expansion_profile_admitted\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
                L",\"graph_expansion_profile\":" + input.graphExpansionProfileJson +
                L",\"graph_expansion_pre_node_count\":" + std::to_wstring(input.graphExpansionPreNodeCount) +
                L",\"graph_expansion_expanded_node_count\":" + std::to_wstring(input.graphExpansionExpandedNodeCount) +
                L",\"graph_expansion_expanded_edge_count\":" + std::to_wstring(input.graphExpansionExpandedEdgeCount) +
                L",\"checkpoint_schema\":\"worker-graph-checkpoint-log-0.1\"" +
                L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
                L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
                L",\"checkpoint_replay\":" + input.checkpointReplayJson +
                L",\"checkpoint_artifacts\":" + input.checkpointArtifactsJson +
                L",\"checkpoints\":" + input.checkpointLogJson +
                L",\"verdict\":" + JsonString(input.verdict) +
                L",\"nodes\":" + input.nodesJson +
                L",\"canonical_nodes\":" + input.canonicalNodesJson +
                L"}";
        }

        std::wstring GraphResultManifestFields(WorkerGraphPublishInput const& input)
        {
            return std::wstring(L",\"job_output_schema\":\"worker-on-device-graph-result-artifact-0.1\"") +
                L",\"graph_id\":" + JsonString(input.graphId) +
                L",\"node_count\":" + std::to_wstring(input.nodeCount) +
                L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
                L",\"verdict\":" + JsonString(input.verdict) +
                L",\"graph_result_sha256\":" + JsonString(input.graphResultSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
                L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
                L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
                L",\"xvm_node_count\":" + std::to_wstring(input.xvmNodeCount);
        }

        std::wstring EvidenceSealInputJson(
            WorkerGraphPublishInput const& input,
            WorkerGraphPublishedArtifact const& graphPublished,
            std::wstring const& claimBoundaryJson)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(WorkerGraphEvidenceSealSchemaVersion()) +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"graph_schema_version\":" + JsonString(input.graphSchemaVersion) +
                L",\"graph_id\":" + JsonString(input.graphId) +
                L",\"graph_spec_sha256\":" + JsonString(input.graphSpecSha256) +
                L",\"node_lineage_sha256\":" + JsonString(input.nodeLineageSha256) +
                L",\"canonical_node_lineage_sha256\":" + JsonString(input.canonicalNodeLineageSha256) +
                L",\"node_count\":" + std::to_wstring(input.nodeCount) +
                L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
                L",\"passed_node_count\":" + std::to_wstring(input.passedNodeCount) +
                L",\"failed_node_count\":" + std::to_wstring(input.failedNodeCount) +
                L",\"graph_result_sha256\":" + JsonString(input.graphResultSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
                L",\"graph_result_artifact_id\":" + JsonString(graphPublished.artifactId) +
                L",\"graph_result_artifact_sha256\":" + JsonString(graphPublished.sha256) +
                L",\"resource_ledger\":" + input.resourceLedgerJson +
                L",\"bounded_loop_profile_admitted\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
                L",\"bounded_loop_profile\":" + input.boundedLoopProfileJson +
                L",\"bounded_loop_iteration_count\":" + std::to_wstring(input.boundedLoopIterationCount) +
                L",\"bounded_loop_max_iterations\":" + std::to_wstring(input.boundedLoopMaxIterations) +
                L",\"bounded_loop_fuel_budget\":" + std::to_wstring(input.boundedLoopFuelBudget) +
                L",\"bounded_loop_fuel_consumed\":" + std::to_wstring(input.boundedLoopFuelConsumed) +
                L",\"static_neighbor_read_profile_admitted\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
                L",\"static_neighbor_read_profile\":" + input.staticNeighborReadProfileJson +
                L",\"static_neighbor_read_neighbor_count\":" + std::to_wstring(input.staticNeighborReadNeighborCount) +
                L",\"graph_expansion_profile_admitted\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
                L",\"graph_expansion_profile\":" + input.graphExpansionProfileJson +
                L",\"graph_expansion_pre_node_count\":" + std::to_wstring(input.graphExpansionPreNodeCount) +
                L",\"graph_expansion_expanded_node_count\":" + std::to_wstring(input.graphExpansionExpandedNodeCount) +
                L",\"graph_expansion_expanded_edge_count\":" + std::to_wstring(input.graphExpansionExpandedEdgeCount) +
                L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
                L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
                L",\"verdict\":" + JsonString(input.verdict) +
                L",\"claim_boundary\":" + claimBoundaryJson +
                L"}";
        }

        std::wstring EvidencePayload(
            WorkerGraphPublishInput const& input,
            WorkerGraphPublishedArtifact const& graphPublished,
            std::wstring const& sealInputJson,
            std::wstring const& evidenceBundleSealSha256,
            std::wstring const& claimBoundaryJson)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(WorkerGraphEvidenceBundleSchemaVersion()) +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"graph_schema_version\":" + JsonString(input.graphSchemaVersion) +
                L",\"runtime_mode\":\"bounded_on_device_graph_v1\"" +
                L",\"programmable_runtime_foundation\":true" +
                L",\"shared_result_envelope_schema\":" + JsonString(WorkerResultEnvelopeSchemaVersion()) +
                L",\"graph_id\":" + JsonString(input.graphId) +
                L",\"graph_spec_sha256\":" + JsonString(input.graphSpecSha256) +
                L",\"node_lineage_sha256\":" + JsonString(input.nodeLineageSha256) +
                L",\"canonical_node_lineage_sha256\":" + JsonString(input.canonicalNodeLineageSha256) +
                L",\"node_count\":" + std::to_wstring(input.nodeCount) +
                L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
                L",\"passed_node_count\":" + std::to_wstring(input.passedNodeCount) +
                L",\"failed_node_count\":" + std::to_wstring(input.failedNodeCount) +
                L",\"total_logical_bytes_read\":" + std::to_wstring(input.totalLogicalBytes) +
                L",\"resource_ledger\":" + input.resourceLedgerJson +
                L",\"bounded_loop_profile_admitted\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
                L",\"bounded_loop_profile\":" + input.boundedLoopProfileJson +
                L",\"bounded_loop_iteration_count\":" + std::to_wstring(input.boundedLoopIterationCount) +
                L",\"bounded_loop_max_iterations\":" + std::to_wstring(input.boundedLoopMaxIterations) +
                L",\"bounded_loop_fuel_budget\":" + std::to_wstring(input.boundedLoopFuelBudget) +
                L",\"bounded_loop_fuel_consumed\":" + std::to_wstring(input.boundedLoopFuelConsumed) +
                L",\"static_neighbor_read_profile_admitted\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
                L",\"static_neighbor_read_profile\":" + input.staticNeighborReadProfileJson +
                L",\"static_neighbor_read_neighbor_count\":" + std::to_wstring(input.staticNeighborReadNeighborCount) +
                L",\"graph_expansion_profile_admitted\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
                L",\"graph_expansion_profile\":" + input.graphExpansionProfileJson +
                L",\"graph_expansion_pre_node_count\":" + std::to_wstring(input.graphExpansionPreNodeCount) +
                L",\"graph_expansion_expanded_node_count\":" + std::to_wstring(input.graphExpansionExpandedNodeCount) +
                L",\"graph_expansion_expanded_edge_count\":" + std::to_wstring(input.graphExpansionExpandedEdgeCount) +
                L",\"checkpoint_schema\":\"worker-graph-checkpoint-log-0.1\"" +
                L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
                L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
                L",\"checkpoint_replay\":" + input.checkpointReplayJson +
                L",\"checkpoint_artifacts\":" + input.checkpointArtifactsJson +
                L",\"checkpoints\":" + input.checkpointLogJson +
                L",\"verdict\":" + JsonString(input.verdict) +
                L",\"graph_result_sha256\":" + JsonString(input.graphResultSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
                L",\"graph_result_canonical_schema\":\"worker-on-device-graph-canonical-result-0.1\"" +
                L",\"graph_result_artifact_id\":" + JsonString(graphPublished.artifactId) +
                L",\"graph_result_artifact_kind\":" + JsonString(graphPublished.artifactKind) +
                L",\"graph_result_artifact_sha256\":" + JsonString(graphPublished.sha256) +
                L",\"graph_result_artifact_bytes\":" + std::to_wstring(graphPublished.bytes) +
                L",\"xvm_node_count\":" + std::to_wstring(input.xvmNodeCount) +
                L",\"worker_evidence_seal_schema\":" + JsonString(WorkerGraphEvidenceSealSchemaVersion()) +
                L",\"worker_evidence_seal_input_json\":" + JsonString(sealInputJson) +
                L",\"worker_evidence_seal_sha256\":" + JsonString(evidenceBundleSealSha256) +
                L",\"node_lineage\":" + input.nodesJson +
                L",\"canonical_node_lineage\":" + input.canonicalNodesJson +
                L",\"claim_boundary\":" + claimBoundaryJson +
                L"}";
        }

        std::wstring EvidenceManifestFields(
            WorkerGraphPublishInput const& input,
            WorkerGraphPublishedArtifact const& graphPublished,
            std::wstring const& evidenceBundleSealSha256)
        {
            return std::wstring(L",\"job_output_schema\":") + JsonString(WorkerGraphEvidenceBundleSchemaVersion()) +
                L",\"graph_id\":" + JsonString(input.graphId) +
                L",\"graph_spec_sha256\":" + JsonString(input.graphSpecSha256) +
                L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
                L",\"graph_result_artifact_sha256\":" + JsonString(graphPublished.sha256) +
                L",\"worker_evidence_seal_sha256\":" + JsonString(evidenceBundleSealSha256) +
                L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
                L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
                L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
                L",\"verdict\":" + JsonString(input.verdict);
        }

        std::wstring EvidenceResponseFields(
            WorkerGraphPublishedArtifact const& evidencePublished,
            std::wstring const& evidenceBundleSealSha256)
        {
            return std::wstring(L",\"evidence_bundle_published\":true") +
                L",\"evidence_bundle_schema\":" + JsonString(WorkerGraphEvidenceBundleSchemaVersion()) +
                L",\"evidence_bundle_artifact_id\":" + JsonString(evidencePublished.artifactId) +
                L",\"evidence_bundle_artifact_kind\":" + JsonString(evidencePublished.artifactKind) +
                L",\"evidence_bundle_artifact_sha256\":" + JsonString(evidencePublished.sha256) +
                L",\"evidence_bundle_artifact_bytes\":" + std::to_wstring(evidencePublished.bytes) +
                L",\"evidence_bundle_seal_schema\":" + JsonString(WorkerGraphEvidenceSealSchemaVersion()) +
                L",\"evidence_bundle_seal_sha256\":" + JsonString(evidenceBundleSealSha256);
        }
    }

    WorkerGraphPublishResult WorkerGraphPublishArtifacts(
        WorkerGraphPublishInput const& input,
        WorkerGraphArtifactPublisher const& publisher,
        WorkerGraphSha256Text const& sha256Text)
    {
        WorkerGraphPublishResult result;
        result.evidenceResponseFields = L",\"evidence_bundle_published\":false";

        auto graphResultArtifactId = WorkerGraphOptionalString(
            input.graph,
            L"graph_result_artifact_id",
            WorkerGraphOptionalString(input.request, L"graph_result_artifact_id"));
        if (graphResultArtifactId.empty())
        {
            graphResultArtifactId = DefaultGraphArtifactId(input.graphId, L"-graph-result");
        }

        JsonObject graphPublishRequest;
        InsertString(graphPublishRequest, L"artifact_id", graphResultArtifactId);
        InsertString(graphPublishRequest, L"artifact_kind", L"on-device-graph-result");
        result.graphResult = publisher(
            graphPublishRequest,
            graphResultArtifactId,
            L"on-device-graph-result",
            WideToUtf8Local(GraphResultPayload(input)),
            GraphResultManifestFields(input));

        auto publishEvidenceBundle = input.request.GetNamedBoolean(L"publish_evidence_bundle", false);
        if (input.graph.HasKey(L"publish_evidence_bundle"))
        {
            publishEvidenceBundle = input.graph.GetNamedBoolean(L"publish_evidence_bundle", publishEvidenceBundle);
        }
        if (!publishEvidenceBundle)
        {
            return result;
        }

        auto evidenceBundleArtifactId = WorkerGraphOptionalString(
            input.graph,
            L"evidence_bundle_artifact_id",
            WorkerGraphOptionalString(input.request, L"evidence_bundle_artifact_id"));
        if (evidenceBundleArtifactId.empty())
        {
            evidenceBundleArtifactId = DefaultGraphArtifactId(input.graphId, L"-evidence-bundle");
        }

        auto evidenceBundleArtifactKind = WorkerGraphOptionalString(
            input.graph,
            L"evidence_bundle_artifact_kind",
            WorkerGraphOptionalString(input.request, L"evidence_bundle_artifact_kind", L"on-device-graph-evidence-bundle"));

        auto claimBoundaryJson = GraphClaimBoundaryJson(input);
        auto sealInputJson = EvidenceSealInputJson(input, result.graphResult, claimBoundaryJson);
        result.evidenceBundleSealSha256 = sha256Text(WideToUtf8Local(sealInputJson));

        JsonObject evidencePublishRequest;
        InsertString(evidencePublishRequest, L"artifact_id", evidenceBundleArtifactId);
        InsertString(evidencePublishRequest, L"artifact_kind", evidenceBundleArtifactKind);
        if (input.graph.HasKey(L"evidence_bundle_rollback_headroom_bytes"))
        {
            InsertNumber(evidencePublishRequest, L"rollback_headroom_bytes", OptionalUInt64(input.graph, L"evidence_bundle_rollback_headroom_bytes"));
        }
        else if (input.request.HasKey(L"evidence_bundle_rollback_headroom_bytes"))
        {
            InsertNumber(evidencePublishRequest, L"rollback_headroom_bytes", OptionalUInt64(input.request, L"evidence_bundle_rollback_headroom_bytes"));
        }
        if (input.graph.HasKey(L"evidence_bundle_workspace_budget_bytes"))
        {
            InsertNumber(evidencePublishRequest, L"workspace_budget_bytes", OptionalUInt64(input.graph, L"evidence_bundle_workspace_budget_bytes"));
        }
        else if (input.request.HasKey(L"evidence_bundle_workspace_budget_bytes"))
        {
            InsertNumber(evidencePublishRequest, L"workspace_budget_bytes", OptionalUInt64(input.request, L"evidence_bundle_workspace_budget_bytes"));
        }

        result.evidenceBundle = publisher(
            evidencePublishRequest,
            evidenceBundleArtifactId,
            L"on-device-graph-evidence-bundle",
            WideToUtf8Local(EvidencePayload(input, result.graphResult, sealInputJson, result.evidenceBundleSealSha256, claimBoundaryJson)),
            EvidenceManifestFields(input, result.graphResult, result.evidenceBundleSealSha256));
        result.evidenceBundlePublished = true;
        result.evidenceResponseFields = EvidenceResponseFields(result.evidenceBundle, result.evidenceBundleSealSha256);
        return result;
    }

    std::wstring WorkerGraphSubmitGraphResponse(
        WorkerGraphPublishInput const& input,
        WorkerGraphPublishResult const& publishResult,
        double elapsedMs)
    {
        return std::wstring(L"{\"ok\":true") +
            L",\"protocol_version\":" + JsonString(input.protocolVersion) +
            L",\"command\":\"submit_graph\"" +
            L",\"schema_version\":\"worker-on-device-graph-result-0.2\"" +
            L",\"graph_schema_version\":" + JsonString(input.graphSchemaVersion) +
            L",\"graph_id\":" + JsonString(input.graphId) +
            L",\"execution_plan_schema\":" + JsonString(input.executionPlanSchemaVersion) +
            L",\"execution_plan_sha256\":" + JsonString(input.executionPlanSha256) +
            L",\"full_graph_pre_admission\":" + std::wstring(input.fullGraphPreAdmission ? L"true" : L"false") +
            L",\"pre_admitted_node_count\":" + std::to_wstring(input.preAdmittedNodeCount) +
            L",\"pre_admitted_edge_count\":" + std::to_wstring(input.preAdmittedEdgeCount) +
            L",\"execution_plan\":" + input.executionPlanJson +
            L",\"runtime_mode\":\"bounded_on_device_graph_v1\"" +
            L",\"programmable_runtime_foundation\":true" +
            L",\"shared_result_envelope_schema\":" + JsonString(WorkerResultEnvelopeSchemaVersion()) +
            L",\"node_count\":" + std::to_wstring(input.nodeCount) +
            L",\"edge_count\":" + std::to_wstring(input.edgeCount) +
            L",\"passed_node_count\":" + std::to_wstring(input.passedNodeCount) +
            L",\"failed_node_count\":" + std::to_wstring(input.failedNodeCount) +
            L",\"total_logical_bytes_read\":" + std::to_wstring(input.totalLogicalBytes) +
            L",\"resource_ledger\":" + input.resourceLedgerJson +
            L",\"bounded_loop_profile_admitted\":" + std::wstring(input.boundedLoopProfileAdmitted ? L"true" : L"false") +
            L",\"bounded_loop_profile\":" + input.boundedLoopProfileJson +
            L",\"bounded_loop_iteration_count\":" + std::to_wstring(input.boundedLoopIterationCount) +
            L",\"bounded_loop_max_iterations\":" + std::to_wstring(input.boundedLoopMaxIterations) +
            L",\"bounded_loop_fuel_budget\":" + std::to_wstring(input.boundedLoopFuelBudget) +
            L",\"bounded_loop_fuel_consumed\":" + std::to_wstring(input.boundedLoopFuelConsumed) +
            L",\"static_neighbor_read_profile_admitted\":" + std::wstring(input.staticNeighborReadProfileAdmitted ? L"true" : L"false") +
            L",\"static_neighbor_read_profile\":" + input.staticNeighborReadProfileJson +
            L",\"static_neighbor_read_neighbor_count\":" + std::to_wstring(input.staticNeighborReadNeighborCount) +
            L",\"graph_expansion_profile_admitted\":" + std::wstring(input.graphExpansionProfileAdmitted ? L"true" : L"false") +
            L",\"graph_expansion_profile\":" + input.graphExpansionProfileJson +
            L",\"graph_expansion_pre_node_count\":" + std::to_wstring(input.graphExpansionPreNodeCount) +
            L",\"graph_expansion_expanded_node_count\":" + std::to_wstring(input.graphExpansionExpandedNodeCount) +
            L",\"graph_expansion_expanded_edge_count\":" + std::to_wstring(input.graphExpansionExpandedEdgeCount) +
            L",\"checkpoint_schema\":\"worker-graph-checkpoint-log-0.1\"" +
            L",\"checkpoint_count\":" + std::to_wstring(input.checkpointCount) +
            L",\"checkpoint_chain_sha256\":" + JsonString(input.checkpointChainSha256) +
            L",\"checkpoint_replay_verified\":" + std::wstring(input.checkpointReplayVerified ? L"true" : L"false") +
            L",\"checkpoint_replay\":" + input.checkpointReplayJson +
            L",\"checkpoint_artifacts\":" + input.checkpointArtifactsJson +
            L",\"graph_result_sha256\":" + JsonString(input.graphResultSha256) +
            L",\"graph_result_canonical_sha256\":" + JsonString(input.graphResultCanonicalSha256) +
            L",\"graph_result_canonical_schema\":\"worker-on-device-graph-canonical-result-0.1\"" +
            L",\"graph_result_artifact_id\":" + JsonString(publishResult.graphResult.artifactId) +
            L",\"graph_result_artifact_kind\":" + JsonString(publishResult.graphResult.artifactKind) +
            L",\"graph_result_artifact_sha256\":" + JsonString(publishResult.graphResult.sha256) +
            L",\"graph_result_artifact_bytes\":" + std::to_wstring(publishResult.graphResult.bytes) +
            L",\"verdict\":" + JsonString(input.verdict) +
            L",\"arbitrary_code_executed\":false" +
            L",\"arbitrary_native_or_host_code_executed\":false" +
            L",\"admitted_xvm_bytecode_executed\":" + std::wstring(input.xvmNodeCount > 0 ? L"true" : L"false") +
            L",\"interpreter_payload_executed\":" + std::wstring(input.xvmNodeCount > 0 ? L"true" : L"false") +
            L",\"runtime_shader_compilation_used\":false" +
            L",\"nodes\":" + input.nodesJson +
            publishResult.evidenceResponseFields +
            L",\"publish_ms\":" + DoubleJsonLocal(publishResult.graphResult.publishMs, 6) +
            L",\"elapsed_ms\":" + DoubleJsonLocal(elapsedMs, 6) +
            L"}";
    }
}
