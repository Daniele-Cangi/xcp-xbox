#include "pch.h"

#include "WorkerGraphExpansion.h"

#include "WorkerGraphRuntime.h"
#include "WorkerGraphValidation.h"
#include "../ProbeResult.h"

#include <algorithm>
#include <sstream>
#include <vector>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        struct ExpansionNodeSpec
        {
            std::wstring nodeId;
            std::wstring kernelId;
            std::wstring resultArtifactId;
            std::wstring edgeRole;
        };

        void InsertString(JsonObject& object, wchar_t const* name, std::wstring const& value)
        {
            object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
        }

        std::wstring StringArrayJson(std::vector<std::wstring> const& values)
        {
            std::wostringstream out;
            out << L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    out << L",";
                }
                out << JsonString(values[index]);
            }
            out << L"]";
            return out.str();
        }

        bool KernelIsAllowlisted(std::wstring const& kernelId)
        {
            return kernelId == L"stream_mix_v1" ||
                kernelId == L"hash_reduce_v1" ||
                kernelId == L"tiled_scan_u8_v1";
        }

        std::wstring BindingMode(JsonObject const& node)
        {
            if (!node.HasKey(L"input_binding"))
            {
                return L"";
            }
            auto bindingValue = node.GetNamedValue(L"input_binding");
            if (bindingValue.ValueType() != JsonValueType::Object)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_target_invalid",
                    "graph expansion target input_binding must be an object");
            }
            return WorkerGraphOptionalString(bindingValue.GetObject(), L"mode");
        }
    }

    WorkerGraphExpansionApplication WorkerGraphApplyBoundedStaticExpansion(
        JsonObject const& graph,
        JsonArray const& nodes)
    {
        WorkerGraphExpansionApplication result;
        result.graph = graph;
        result.nodes = nodes;
        result.preNodeCount = static_cast<uint64_t>(nodes.Size());

        if (!graph.HasKey(L"graph_expansion_profile"))
        {
            return result;
        }

        auto profileValue = graph.GetNamedValue(L"graph_expansion_profile");
        if (profileValue.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_profile_invalid",
                "graph_expansion_profile must be a JSON object");
        }
        auto profile = profileValue.GetObject();
        auto schema = WorkerGraphOptionalString(profile, L"schema_version");
        if (schema != L"worker-graph-bounded-expansion-profile-v1")
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_profile_invalid",
                "graph expansion profile schema_version is invalid");
        }

        result.profileId = WorkerGraphOptionalString(profile, L"profile_id");
        if (result.profileId != L"bounded_static_fanin_expansion_v1")
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_profile_invalid",
                "graph expansion profile_id is not allowlisted");
        }
        result.targetNodeId = WorkerGraphOptionalString(profile, L"target_node_id");
        result.manifestArtifactId = WorkerGraphOptionalString(profile, L"manifest_artifact_id");
        if (!WorkerGraphIsSafeId(result.targetNodeId) || !WorkerGraphIsSafeId(result.manifestArtifactId))
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_profile_invalid",
                "graph expansion target and manifest ids must be safe ids");
        }
        if (!profile.HasKey(L"expanded_nodes") ||
            profile.GetNamedValue(L"expanded_nodes").ValueType() != JsonValueType::Array)
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_count_invalid",
                "graph expansion requires an expanded_nodes array");
        }

        auto expandedNodeValues = profile.GetNamedArray(L"expanded_nodes");
        if (expandedNodeValues.Size() == 0 || expandedNodeValues.Size() > WorkerGraphMaxExpansionNodes())
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_count_invalid",
                "graph expansion node count is outside the admitted range");
        }
        if (static_cast<uint64_t>(nodes.Size()) + expandedNodeValues.Size() > WorkerMaxOnDeviceGraphNodes())
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_count_invalid",
                "expanded graph would exceed the on-device graph node limit");
        }

        std::vector<std::wstring> existingNodeIds;
        std::vector<JsonObject> existingNodes;
        std::vector<uint32_t> existingNodeIndices;
        existingNodeIds.reserve(nodes.Size());
        existingNodes.reserve(nodes.Size());
        existingNodeIndices.reserve(nodes.Size());
        for (uint32_t index = 0; index < nodes.Size(); ++index)
        {
            auto value = nodes.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                continue;
            }
            auto node = value.GetObject();
            existingNodeIds.push_back(WorkerGraphOptionalString(node, L"node_id", WorkerGraphOptionalString(node, L"id")));
            existingNodes.push_back(node);
            existingNodeIndices.push_back(index);
        }

        auto findExistingIndex = [&](std::wstring const& nodeId) -> int64_t
        {
            for (size_t index = 0; index < existingNodeIds.size(); ++index)
            {
                if (existingNodeIds[index] == nodeId)
                {
                    return static_cast<int64_t>(index);
                }
            }
            return -1;
        };

        auto targetIndex = findExistingIndex(result.targetNodeId);
        if (targetIndex < 0)
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_target_missing",
                "graph expansion target node is missing");
        }
        auto targetNode = existingNodes[static_cast<size_t>(targetIndex)];
        auto targetOriginalIndex = existingNodeIndices[static_cast<size_t>(targetIndex)];
        if (WorkerGraphOptionalString(targetNode, L"command") != L"reduce_graph_verdict" ||
            BindingMode(targetNode) != L"mix_bound_input_edges_v1")
        {
            throw WorkerGraphValidationError(
                "submit_graph.expansion_target_invalid",
                "graph expansion target must be a bound reduce_graph_verdict node");
        }

        JsonArray targetEdges;
        std::vector<std::wstring> edgeRoles;
        if (targetNode.HasKey(L"input_edges"))
        {
            auto edgesValue = targetNode.GetNamedValue(L"input_edges");
            if (edgesValue.ValueType() != JsonValueType::Array)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_target_invalid",
                    "graph expansion target input_edges must be an array");
            }
            auto existingEdges = edgesValue.GetArray();
            for (uint32_t index = 0; index < existingEdges.Size(); ++index)
            {
                auto edgeValue = existingEdges.GetAt(index);
                if (edgeValue.ValueType() == JsonValueType::Object)
                {
                    auto edge = edgeValue.GetObject();
                    edgeRoles.push_back(WorkerGraphOptionalString(edge, L"role", L"dependency"));
                }
                targetEdges.Append(edgeValue);
            }
        }

        std::vector<ExpansionNodeSpec> expandedSpecs;
        std::vector<std::wstring> expandedNodeIds;
        std::vector<std::wstring> expandedResultArtifactIds;
        expandedSpecs.reserve(expandedNodeValues.Size());
        expandedNodeIds.reserve(expandedNodeValues.Size());
        expandedResultArtifactIds.reserve(expandedNodeValues.Size());
        for (uint32_t index = 0; index < expandedNodeValues.Size(); ++index)
        {
            auto nodeValue = expandedNodeValues.GetAt(index);
            if (nodeValue.ValueType() != JsonValueType::Object)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_profile_invalid",
                    "expanded_nodes entries must be objects");
            }
            auto entry = nodeValue.GetObject();
            ExpansionNodeSpec spec;
            spec.nodeId = WorkerGraphOptionalString(entry, L"node_id");
            spec.kernelId = WorkerGraphOptionalString(entry, L"kernel_id");
            spec.resultArtifactId = WorkerGraphOptionalString(entry, L"result_artifact_id");
            spec.edgeRole = WorkerGraphOptionalString(entry, L"edge_role", L"expanded_" + std::to_wstring(index));
            if (!WorkerGraphIsSafeId(spec.nodeId) ||
                !WorkerGraphIsSafeId(spec.kernelId) ||
                !WorkerGraphIsSafeId(spec.resultArtifactId) ||
                !WorkerGraphIsSafeId(spec.edgeRole))
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_profile_invalid",
                    "expanded node ids, kernels, results and roles must be safe ids");
            }
            if (!KernelIsAllowlisted(spec.kernelId))
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_kernel_invalid",
                    "expanded node kernel is not allowlisted for graph expansion");
            }
            if (findExistingIndex(spec.nodeId) >= 0 ||
                std::find(expandedNodeIds.begin(), expandedNodeIds.end(), spec.nodeId) != expandedNodeIds.end() ||
                std::find(expandedResultArtifactIds.begin(), expandedResultArtifactIds.end(), spec.resultArtifactId) != expandedResultArtifactIds.end() ||
                std::find(edgeRoles.begin(), edgeRoles.end(), spec.edgeRole) != edgeRoles.end())
            {
                throw WorkerGraphValidationError(
                    "submit_graph.expansion_duplicate_id",
                    "expanded node ids, result artifacts and roles must be unique");
            }
            expandedNodeIds.push_back(spec.nodeId);
            expandedResultArtifactIds.push_back(spec.resultArtifactId);
            edgeRoles.push_back(spec.edgeRole);
            expandedSpecs.push_back(spec);
        }

        JsonArray expandedNodes;
        for (uint32_t index = 0; index < nodes.Size(); ++index)
        {
            if (index == targetOriginalIndex)
            {
                for (auto const& spec : expandedSpecs)
                {
                    JsonObject expandedNode;
                    InsertString(expandedNode, L"node_id", spec.nodeId);
                    InsertString(expandedNode, L"command", L"run_artifact_manifest_compute_job");
                    InsertString(expandedNode, L"kernel_id", spec.kernelId);
                    InsertString(expandedNode, L"manifest_artifact_id", result.manifestArtifactId);
                    InsertString(expandedNode, L"result_artifact_id", spec.resultArtifactId);
                    InsertString(expandedNode, L"artifact_kind", L"m151-graph-expansion-node-result");
                    expandedNodes.Append(expandedNode);

                    JsonObject edge;
                    InsertString(edge, L"from_node_id", spec.nodeId);
                    InsertString(edge, L"role", spec.edgeRole);
                    InsertString(edge, L"edge_type", L"artifact");
                    InsertString(edge, L"value_type", L"xcp.artifact.digest.v1");
                    InsertString(edge, L"expected_result_artifact_id", spec.resultArtifactId);
                    targetEdges.Append(edge);
                }
                targetNode.Insert(L"input_edges", targetEdges);
                expandedNodes.Append(targetNode);
            }
            else
            {
                expandedNodes.Append(nodes.GetAt(index));
            }
        }

        result.graph.Insert(L"nodes", expandedNodes);
        result.nodes = expandedNodes;
        result.expandedNodeCount = static_cast<uint64_t>(expandedSpecs.size());
        result.expandedEdgeCount = static_cast<uint64_t>(expandedSpecs.size());
        result.profileJson =
            std::wstring(L"{\"schema_version\":\"worker-graph-expansion-profile-0.1\"") +
            L",\"profile_id\":" + JsonString(result.profileId) +
            L",\"target_node_id\":" + JsonString(result.targetNodeId) +
            L",\"manifest_artifact_id\":" + JsonString(result.manifestArtifactId) +
            L",\"pre_node_count\":" + std::to_wstring(result.preNodeCount) +
            L",\"expanded_node_count\":" + std::to_wstring(result.expandedNodeCount) +
            L",\"expanded_edge_count\":" + std::to_wstring(result.expandedEdgeCount) +
            L",\"max_expansion_nodes\":" + std::to_wstring(WorkerGraphMaxExpansionNodes()) +
            L",\"expanded_node_ids\":" + StringArrayJson(expandedNodeIds) +
            L",\"admitted\":true}";
        result.present = true;
        return result;
    }
}
