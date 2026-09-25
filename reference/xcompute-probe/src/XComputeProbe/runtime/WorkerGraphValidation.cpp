#include "pch.h"
#include "WorkerGraphValidation.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphRuntime.h"
#include "../ProbeResult.h"

using namespace winrt::Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        uint64_t ReadLoopUInt64(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t fallback,
            uint64_t minimum,
            uint64_t maximum,
            char const* code)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto raw = object.GetNamedNumber(name);
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < static_cast<double>(minimum) || raw > static_cast<double>(maximum))
            {
                throw WorkerGraphValidationError(code, "integer field is outside the admitted range");
            }
            return static_cast<uint64_t>(raw);
        }

        std::wstring LoopStringArrayJson(std::vector<std::wstring> const& values)
        {
            std::vector<std::wstring> items;
            items.reserve(values.size());
            for (auto const& value : values)
            {
                items.push_back(JsonString(value));
            }
            return WorkerGraphJsonArray(items);
        }

        bool LoopNodeHasEdge(
            JsonObject const& node,
            std::wstring const& fromNodeId,
            std::wstring const& role)
        {
            if (!node.HasKey(L"input_edges"))
            {
                return false;
            }
            auto edgesValue = node.GetNamedValue(L"input_edges");
            if (edgesValue.ValueType() != JsonValueType::Array)
            {
                return false;
            }
            auto edges = edgesValue.GetArray();
            for (uint32_t index = 0; index < edges.Size(); ++index)
            {
                auto value = edges.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    continue;
                }
                auto edge = value.GetObject();
                auto edgeFrom = WorkerGraphOptionalString(edge, L"from_node_id", WorkerGraphOptionalString(edge, L"from"));
                auto edgeRole = WorkerGraphOptionalString(edge, L"role", L"dependency");
                if (edgeFrom == fromNodeId && edgeRole == role)
                {
                    return true;
                }
            }
            return false;
        }

        std::wstring LoopNodeBindingMode(JsonObject const& node)
        {
            if (!node.HasKey(L"input_binding"))
            {
                return L"";
            }
            auto bindingValue = node.GetNamedValue(L"input_binding");
            if (bindingValue.ValueType() != JsonValueType::Object)
            {
                throw WorkerGraphValidationError("submit_graph.loop_binding_invalid", "bounded loop iteration binding must be an object");
            }
            return WorkerGraphOptionalString(bindingValue.GetObject(), L"mode");
        }

        struct NeighborRoleEdge
        {
            std::wstring role;
            std::wstring fromNodeId;
            std::wstring edgeType;
        };

        std::vector<NeighborRoleEdge> ReadNeighborRoleEdges(JsonObject const& node)
        {
            std::vector<NeighborRoleEdge> roleEdges;
            if (!node.HasKey(L"input_edges") || node.GetNamedValue(L"input_edges").ValueType() != JsonValueType::Array)
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_edge_invalid", "static neighbor-read target requires input_edges");
            }

            auto edges = node.GetNamedArray(L"input_edges");
            roleEdges.reserve(edges.Size());
            for (uint32_t index = 0; index < edges.Size(); ++index)
            {
                auto value = edges.GetAt(index);
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw WorkerGraphValidationError("submit_graph.neighbor_edge_invalid", "static neighbor-read input edges must be objects");
                }
                auto edge = value.GetObject();
                NeighborRoleEdge parsed;
                parsed.fromNodeId = WorkerGraphOptionalString(edge, L"from_node_id", WorkerGraphOptionalString(edge, L"from"));
                parsed.role = WorkerGraphOptionalString(edge, L"role", L"dependency");
                parsed.edgeType = WorkerGraphOptionalString(edge, L"edge_type", L"artifact");
                roleEdges.push_back(parsed);
            }
            return roleEdges;
        }
    }

    WorkerGraphValidationError::WorkerGraphValidationError(std::string codeValue, std::string messageValue) :
        code(codeValue),
        message(messageValue)
    {
    }

    char const* WorkerGraphValidationError::what() const noexcept
    {
        return message.c_str();
    }

    std::wstring WorkerGraphOptionalString(JsonObject const& object, wchar_t const* name, std::wstring const& fallback)
    {
        if (!object.HasKey(name))
        {
            return fallback;
        }
        auto value = object.GetNamedString(name, fallback);
        return std::wstring(value.data(), value.size());
    }

    bool WorkerGraphIsSafeId(std::wstring const& value)
    {
        if (value.empty() || value.size() > 64)
        {
            return false;
        }
        for (auto ch : value)
        {
            bool ok = (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'_' ||
                ch == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    WorkerGraphRequestValidation WorkerGraphValidateRequest(
        JsonObject const& request,
        std::wstring const& generatedGraphId,
        uint32_t maxNodeCount)
    {
        WorkerGraphRequestValidation validation;
        validation.graph = request;
        if (request.HasKey(L"graph"))
        {
            auto graphValue = request.GetNamedValue(L"graph");
            if (graphValue.ValueType() != JsonValueType::Object)
            {
                throw WorkerGraphValidationError("submit_graph.graph_invalid", "graph must be a JSON object");
            }
            validation.graph = graphValue.GetObject();
        }

        auto schema = WorkerGraphOptionalString(validation.graph, L"schema_version");
        if (schema != L"worker-on-device-graph-v1")
        {
            throw WorkerGraphValidationError("submit_graph.schema_invalid", "graph schema_version must be worker-on-device-graph-v1");
        }

        validation.graphId = WorkerGraphOptionalString(validation.graph, L"graph_id", WorkerGraphOptionalString(request, L"graph_id"));
        if (validation.graphId.empty())
        {
            validation.graphId = generatedGraphId;
        }
        if (!WorkerGraphIsSafeId(validation.graphId))
        {
            throw WorkerGraphValidationError("submit_graph.graph_id_invalid", "graph_id must be 1..64 chars using letters, digits, underscore, or dash");
        }
        if (!validation.graph.HasKey(L"nodes"))
        {
            throw WorkerGraphValidationError("submit_graph.nodes_required", "graph nodes array is required");
        }

        validation.nodes = validation.graph.GetNamedArray(L"nodes");
        if (validation.nodes.Size() == 0 || validation.nodes.Size() > maxNodeCount)
        {
            throw WorkerGraphValidationError("submit_graph.node_count_invalid", "graph must contain 1..8 nodes");
        }
        return validation;
    }

    WorkerGraphNodeValidation WorkerGraphValidateNodeValue(IJsonValue const& value, uint32_t nodeIndex, std::wstring const& graphId)
    {
        if (value.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError("submit_graph.node_invalid", "graph node entries must be JSON objects");
        }

        WorkerGraphNodeValidation validation;
        validation.node = value.GetObject();
        validation.nodeId = WorkerGraphOptionalString(validation.node, L"node_id", WorkerGraphOptionalString(validation.node, L"id"));
        if (!WorkerGraphIsSafeId(validation.nodeId))
        {
            throw WorkerGraphValidationError("submit_graph.node_id_invalid", "node_id must be 1..64 chars using letters, digits, underscore, or dash");
        }

        validation.resultArtifactId = WorkerGraphOptionalString(
            validation.node,
            L"result_artifact_id",
            WorkerGraphOptionalString(validation.node, L"artifact_id"));
        if (validation.resultArtifactId.empty())
        {
            auto prefix = graphId;
            if (prefix.size() > 44)
            {
                prefix.resize(44);
            }
            validation.resultArtifactId = prefix + L"-n" + std::to_wstring(nodeIndex) + L"-result";
        }
        return validation;
    }

    void WorkerGraphValidateUniqueNodeId(bool duplicate)
    {
        if (duplicate)
        {
            throw WorkerGraphValidationError("submit_graph.node_id_duplicate", "node_id values must be unique within a graph");
        }
    }

    WorkerGraphInputEdgesValidation WorkerGraphValidateInputEdgesArray(JsonObject const& node)
    {
        WorkerGraphInputEdgesValidation validation;
        if (!node.HasKey(L"input_edges"))
        {
            return validation;
        }

        auto edgeValue = node.GetNamedValue(L"input_edges");
        if (edgeValue.ValueType() != JsonValueType::Array)
        {
            throw WorkerGraphValidationError("submit_graph.input_edges_invalid", "input_edges must be an array");
        }

        validation.present = true;
        validation.edges = edgeValue.GetArray();
        return validation;
    }

    WorkerGraphInputEdgeValidation WorkerGraphValidateInputEdgeSource(
        IJsonValue const& value,
        uint64_t currentEdgeCount,
        uint32_t maxEdgeCount)
    {
        if (currentEdgeCount >= maxEdgeCount)
        {
            throw WorkerGraphValidationError("submit_graph.edge_count_invalid", "graph input edge count exceeds the supported limit");
        }

        if (value.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError("submit_graph.input_edge_invalid", "input_edges entries must be JSON objects");
        }

        auto edge = value.GetObject();
        WorkerGraphInputEdgeValidation validation;
        validation.fromNodeId = WorkerGraphOptionalString(edge, L"from_node_id", WorkerGraphOptionalString(edge, L"from"));
        if (!WorkerGraphIsSafeId(validation.fromNodeId))
        {
            throw WorkerGraphValidationError("submit_graph.edge_from_node_invalid", "input edge from_node_id must reference a safe prior node id");
        }
        validation.expectedArtifactId = WorkerGraphOptionalString(
            edge,
            L"expected_result_artifact_id",
            WorkerGraphOptionalString(edge, L"result_artifact_id"));
        validation.role = WorkerGraphOptionalString(edge, L"role", L"dependency");
        validation.edgeType = WorkerGraphOptionalString(edge, L"edge_type", L"artifact");
        validation.valueType = WorkerGraphOptionalString(
            edge,
            L"value_type",
            validation.edgeType == L"control" ? L"xcp.control.verdict.v1" : L"xcp.artifact.digest.v1");
        validation.edgeKey = validation.fromNodeId + L"\x1f" + validation.role;
        return validation;
    }

    void WorkerGraphValidateInputEdgeRole(
        WorkerGraphInputEdgeValidation const& edge,
        std::vector<std::wstring> const& existingEdgeKeys)
    {
        if (!WorkerGraphIsSafeId(edge.role))
        {
            throw WorkerGraphValidationError("submit_graph.edge_role_invalid", "input edge role must use letters, digits, underscore, or dash");
        }
        if (edge.edgeType != L"artifact" && edge.edgeType != L"control")
        {
            throw WorkerGraphValidationError("submit_graph.edge_type_invalid", "input edge edge_type must be artifact or control");
        }
        if ((edge.edgeType == L"artifact" && edge.valueType != L"xcp.artifact.digest.v1") ||
            (edge.edgeType == L"control" && edge.valueType != L"xcp.control.verdict.v1"))
        {
            throw WorkerGraphValidationError("submit_graph.edge_value_type_invalid", "input edge value_type is incompatible with edge_type");
        }
        if (edge.edgeType == L"control" && !edge.expectedArtifactId.empty())
        {
            throw WorkerGraphValidationError("submit_graph.control_edge_artifact_invalid", "control edges cannot declare expected_result_artifact_id");
        }
        if (std::find(existingEdgeKeys.begin(), existingEdgeKeys.end(), edge.edgeKey) != existingEdgeKeys.end())
        {
            throw WorkerGraphValidationError("submit_graph.edge_duplicate", "input edge from_node_id plus role must be unique for a node");
        }
    }

    std::wstring WorkerGraphValidateInputBindingMode(JsonObject const& node, bool hasResolvedInputEdges)
    {
        if (!node.HasKey(L"input_binding"))
        {
            return L"";
        }

        auto bindingValue = node.GetNamedValue(L"input_binding");
        if (bindingValue.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError("submit_graph.input_binding_invalid", "input_binding must be a JSON object");
        }

        auto binding = bindingValue.GetObject();
        auto inputBindingMode = WorkerGraphOptionalString(binding, L"mode");
        if (inputBindingMode != L"mix_bound_input_edges_v1" &&
            inputBindingMode != L"typed_xvm_inputs_v1" &&
            inputBindingMode != L"control_select_v1" &&
            inputBindingMode != L"static_neighbor_read_v1")
        {
            throw WorkerGraphValidationError("submit_graph.input_binding_mode_invalid", "input_binding mode is not allowlisted");
        }
        if (!hasResolvedInputEdges)
        {
            throw WorkerGraphValidationError("submit_graph.input_binding_incompatible", "mix_bound_input_edges_v1 requires at least one resolved input edge");
        }
        return inputBindingMode;
    }

    WorkerGraphBoundedLoopValidation WorkerGraphValidateBoundedLoopProfile(
        JsonObject const& graph,
        JsonArray const& nodes)
    {
        WorkerGraphBoundedLoopValidation validation;
        if (!graph.HasKey(L"bounded_loop_profile"))
        {
            return validation;
        }

        auto profileValue = graph.GetNamedValue(L"bounded_loop_profile");
        if (profileValue.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError("submit_graph.loop_profile_invalid", "bounded_loop_profile must be a JSON object");
        }
        auto profile = profileValue.GetObject();
        auto schema = WorkerGraphOptionalString(profile, L"schema_version");
        if (schema != L"worker-graph-bounded-loop-profile-v1")
        {
            throw WorkerGraphValidationError("submit_graph.loop_profile_invalid", "bounded loop profile schema_version is invalid");
        }
        validation.profileId = WorkerGraphOptionalString(profile, L"profile_id");
        if (validation.profileId != L"bounded_static_loop_v1")
        {
            throw WorkerGraphValidationError("submit_graph.loop_profile_invalid", "bounded loop profile_id is not allowlisted");
        }
        validation.loopId = WorkerGraphOptionalString(profile, L"loop_id", L"loop");
        validation.initialNodeId = WorkerGraphOptionalString(profile, L"initial_node_id");
        validation.finalNodeId = WorkerGraphOptionalString(profile, L"final_node_id");
        validation.carryEdgeRole = WorkerGraphOptionalString(profile, L"carry_edge_role", L"loop_state");
        validation.finalEdgeRole = WorkerGraphOptionalString(profile, L"final_edge_role", L"loop_result");
        if (!WorkerGraphIsSafeId(validation.loopId) ||
            !WorkerGraphIsSafeId(validation.initialNodeId) ||
            !WorkerGraphIsSafeId(validation.finalNodeId) ||
            !WorkerGraphIsSafeId(validation.carryEdgeRole) ||
            !WorkerGraphIsSafeId(validation.finalEdgeRole))
        {
            throw WorkerGraphValidationError("submit_graph.loop_profile_invalid", "bounded loop ids and roles must use safe ids");
        }

        validation.iterationCount = ReadLoopUInt64(
            profile,
            L"iteration_count",
            0,
            1,
            WorkerGraphMaxBoundedLoopIterations(),
            "submit_graph.loop_iterations_invalid");
        validation.maxIterations = ReadLoopUInt64(
            profile,
            L"max_iterations",
            validation.iterationCount,
            validation.iterationCount,
            WorkerGraphMaxBoundedLoopIterations(),
            "submit_graph.loop_iterations_invalid");
        validation.fuelPerIteration = ReadLoopUInt64(
            profile,
            L"fuel_per_iteration",
            0,
            1,
            1024,
            "submit_graph.loop_fuel_invalid");
        validation.fuelBudget = ReadLoopUInt64(
            profile,
            L"fuel_budget",
            0,
            1,
            WorkerGraphMaxFuel(),
            "submit_graph.loop_fuel_invalid");
        if (validation.fuelBudget != validation.iterationCount * validation.fuelPerIteration)
        {
            throw WorkerGraphValidationError("submit_graph.loop_fuel_invalid", "bounded loop fuel_budget must equal iteration_count * fuel_per_iteration");
        }

        if (!profile.HasKey(L"iteration_node_ids") ||
            profile.GetNamedValue(L"iteration_node_ids").ValueType() != JsonValueType::Array)
        {
            throw WorkerGraphValidationError("submit_graph.loop_iterations_invalid", "bounded loop iteration_node_ids array is required");
        }
        auto iterationIds = profile.GetNamedArray(L"iteration_node_ids");
        if (iterationIds.Size() != validation.iterationCount)
        {
            throw WorkerGraphValidationError("submit_graph.loop_iterations_invalid", "bounded loop iteration_node_ids must match iteration_count");
        }

        std::vector<std::wstring> graphNodeIds;
        std::vector<JsonObject> graphNodes;
        graphNodeIds.reserve(nodes.Size());
        graphNodes.reserve(nodes.Size());
        for (uint32_t index = 0; index < nodes.Size(); ++index)
        {
            auto value = nodes.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                continue;
            }
            auto node = value.GetObject();
            graphNodeIds.push_back(WorkerGraphOptionalString(node, L"node_id", WorkerGraphOptionalString(node, L"id")));
            graphNodes.push_back(node);
        }

        auto findIndex = [&](std::wstring const& nodeId) -> int64_t
        {
            for (size_t index = 0; index < graphNodeIds.size(); ++index)
            {
                if (graphNodeIds[index] == nodeId)
                {
                    return static_cast<int64_t>(index);
                }
            }
            return -1;
        };

        auto initialIndex = findIndex(validation.initialNodeId);
        auto finalIndex = findIndex(validation.finalNodeId);
        if (initialIndex < 0 || finalIndex < 0)
        {
            throw WorkerGraphValidationError("submit_graph.loop_node_missing", "bounded loop initial or final node is missing");
        }

        int64_t previousIndex = initialIndex;
        std::wstring previousNodeId = validation.initialNodeId;
        for (uint32_t index = 0; index < iterationIds.Size(); ++index)
        {
            auto value = iterationIds.GetAt(index);
            if (value.ValueType() != JsonValueType::String)
            {
                throw WorkerGraphValidationError("submit_graph.loop_iterations_invalid", "bounded loop iteration node ids must be strings");
            }
            auto nodeId = std::wstring(value.GetString().c_str());
            if (!WorkerGraphIsSafeId(nodeId) ||
                std::find(validation.iterationNodeIds.begin(), validation.iterationNodeIds.end(), nodeId) != validation.iterationNodeIds.end())
            {
                throw WorkerGraphValidationError("submit_graph.loop_iterations_invalid", "bounded loop iteration node ids must be safe and unique");
            }
            auto nodeIndex = findIndex(nodeId);
            if (nodeIndex < 0)
            {
                throw WorkerGraphValidationError("submit_graph.loop_node_missing", "bounded loop iteration node is missing");
            }
            if (nodeIndex <= previousIndex)
            {
                throw WorkerGraphValidationError("submit_graph.loop_order_invalid", "bounded loop iteration nodes must be topologically ordered");
            }
            auto node = graphNodes[static_cast<size_t>(nodeIndex)];
            if (WorkerGraphOptionalString(node, L"command") != L"run_artifact_manifest_compute_job")
            {
                throw WorkerGraphValidationError("submit_graph.loop_node_command_invalid", "bounded loop iteration nodes must use run_artifact_manifest_compute_job");
            }
            if (LoopNodeBindingMode(node) != L"mix_bound_input_edges_v1")
            {
                throw WorkerGraphValidationError("submit_graph.loop_binding_invalid", "bounded loop iteration nodes must bind mix_bound_input_edges_v1");
            }
            if (!LoopNodeHasEdge(node, previousNodeId, validation.carryEdgeRole))
            {
                throw WorkerGraphValidationError("submit_graph.loop_carry_edge_invalid", "bounded loop iteration must carry state from the previous loop step");
            }
            validation.iterationNodeIds.push_back(nodeId);
            previousIndex = nodeIndex;
            previousNodeId = nodeId;
        }

        if (finalIndex <= previousIndex)
        {
            throw WorkerGraphValidationError("submit_graph.loop_order_invalid", "bounded loop final node must follow the iteration nodes");
        }
        auto finalNode = graphNodes[static_cast<size_t>(finalIndex)];
        if (!LoopNodeHasEdge(finalNode, previousNodeId, validation.finalEdgeRole))
        {
            throw WorkerGraphValidationError("submit_graph.loop_final_edge_invalid", "bounded loop final node must consume the last iteration result");
        }

        validation.profileJson =
            std::wstring(L"{\"schema_version\":\"worker-graph-bounded-loop-profile-0.1\"") +
            L",\"profile_id\":" + JsonString(validation.profileId) +
            L",\"loop_id\":" + JsonString(validation.loopId) +
            L",\"initial_node_id\":" + JsonString(validation.initialNodeId) +
            L",\"final_node_id\":" + JsonString(validation.finalNodeId) +
            L",\"carry_edge_role\":" + JsonString(validation.carryEdgeRole) +
            L",\"final_edge_role\":" + JsonString(validation.finalEdgeRole) +
            L",\"iteration_count\":" + std::to_wstring(validation.iterationCount) +
            L",\"max_iterations\":" + std::to_wstring(validation.maxIterations) +
            L",\"fuel_per_iteration\":" + std::to_wstring(validation.fuelPerIteration) +
            L",\"fuel_budget\":" + std::to_wstring(validation.fuelBudget) +
            L",\"iteration_node_ids\":" + LoopStringArrayJson(validation.iterationNodeIds) +
            L",\"admitted\":true}";
        validation.present = true;
        return validation;
    }

    WorkerGraphStaticNeighborReadValidation WorkerGraphValidateStaticNeighborReadProfile(
        JsonObject const& graph,
        JsonArray const& nodes)
    {
        WorkerGraphStaticNeighborReadValidation validation;
        if (!graph.HasKey(L"static_neighbor_read_profile"))
        {
            return validation;
        }

        auto profileValue = graph.GetNamedValue(L"static_neighbor_read_profile");
        if (profileValue.ValueType() != JsonValueType::Object)
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_profile_invalid", "static_neighbor_read_profile must be a JSON object");
        }
        auto profile = profileValue.GetObject();
        auto schema = WorkerGraphOptionalString(profile, L"schema_version");
        if (schema != L"worker-graph-static-neighbor-read-profile-v1")
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_profile_invalid", "static neighbor-read profile schema_version is invalid");
        }
        validation.profileId = WorkerGraphOptionalString(profile, L"profile_id");
        if (validation.profileId != L"static_neighbor_read_v1")
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_profile_invalid", "static neighbor-read profile_id is not allowlisted");
        }
        validation.targetNodeId = WorkerGraphOptionalString(profile, L"target_node_id");
        if (!WorkerGraphIsSafeId(validation.targetNodeId))
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_profile_invalid", "static neighbor-read target_node_id must be safe");
        }
        if (!profile.HasKey(L"required_neighbor_roles") ||
            profile.GetNamedValue(L"required_neighbor_roles").ValueType() != JsonValueType::Array)
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read required_neighbor_roles array is required");
        }
        auto roles = profile.GetNamedArray(L"required_neighbor_roles");
        if (roles.Size() == 0 || roles.Size() > WorkerGraphMaxStaticNeighborReadEdges())
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read role count is outside the admitted range");
        }
        for (uint32_t index = 0; index < roles.Size(); ++index)
        {
            auto value = roles.GetAt(index);
            if (value.ValueType() != JsonValueType::String)
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read roles must be strings");
            }
            auto role = std::wstring(value.GetString().c_str());
            if (!WorkerGraphIsSafeId(role) ||
                std::find(validation.requiredNeighborRoles.begin(), validation.requiredNeighborRoles.end(), role) != validation.requiredNeighborRoles.end())
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read roles must be safe and unique");
            }
            validation.requiredNeighborRoles.push_back(role);
        }
        validation.neighborCount = validation.requiredNeighborRoles.size();
        validation.maxNeighbors = ReadLoopUInt64(
            profile,
            L"max_neighbors",
            validation.neighborCount,
            validation.neighborCount,
            WorkerGraphMaxStaticNeighborReadEdges(),
            "submit_graph.neighbor_count_invalid");

        std::vector<std::wstring> graphNodeIds;
        std::vector<JsonObject> graphNodes;
        graphNodeIds.reserve(nodes.Size());
        graphNodes.reserve(nodes.Size());
        for (uint32_t index = 0; index < nodes.Size(); ++index)
        {
            auto value = nodes.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                continue;
            }
            auto node = value.GetObject();
            graphNodeIds.push_back(WorkerGraphOptionalString(node, L"node_id", WorkerGraphOptionalString(node, L"id")));
            graphNodes.push_back(node);
        }

        auto findIndex = [&](std::wstring const& nodeId) -> int64_t
        {
            for (size_t index = 0; index < graphNodeIds.size(); ++index)
            {
                if (graphNodeIds[index] == nodeId)
                {
                    return static_cast<int64_t>(index);
                }
            }
            return -1;
        };

        auto targetIndex = findIndex(validation.targetNodeId);
        if (targetIndex < 0)
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_node_missing", "static neighbor-read target node is missing");
        }
        auto targetNode = graphNodes[static_cast<size_t>(targetIndex)];
        if (WorkerGraphOptionalString(targetNode, L"command") != L"neighbor_read")
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_node_command_invalid", "static neighbor-read target node must use neighbor_read");
        }
        if (LoopNodeBindingMode(targetNode) != L"static_neighbor_read_v1")
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_binding_invalid", "static neighbor-read target must bind static_neighbor_read_v1");
        }

        auto targetEdges = ReadNeighborRoleEdges(targetNode);
        if (targetEdges.size() > WorkerGraphMaxStaticNeighborReadEdges())
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read target has too many neighbor edges");
        }
        if (targetEdges.size() > validation.maxNeighbors)
        {
            throw WorkerGraphValidationError("submit_graph.neighbor_count_invalid", "static neighbor-read target exceeds profile max_neighbors");
        }
        std::vector<std::wstring> presentRoles;
        for (auto const& edge : targetEdges)
        {
            if (!WorkerGraphIsSafeId(edge.role) || !WorkerGraphIsSafeId(edge.fromNodeId) || edge.edgeType != L"artifact")
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_edge_invalid", "static neighbor-read edges must be safe artifact edges");
            }
            auto sourceIndex = findIndex(edge.fromNodeId);
            if (sourceIndex < 0 || sourceIndex >= targetIndex)
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_edge_invalid", "static neighbor-read edges must point to prior graph nodes");
            }
            if (std::find(validation.requiredNeighborRoles.begin(), validation.requiredNeighborRoles.end(), edge.role) != validation.requiredNeighborRoles.end() &&
                std::find(presentRoles.begin(), presentRoles.end(), edge.role) == presentRoles.end())
            {
                presentRoles.push_back(edge.role);
            }
        }

        for (auto const& role : validation.requiredNeighborRoles)
        {
            if (std::find(presentRoles.begin(), presentRoles.end(), role) == presentRoles.end())
            {
                throw WorkerGraphValidationError("submit_graph.neighbor_role_missing", "static neighbor-read target is missing a required neighbor role");
            }
        }

        validation.profileJson =
            std::wstring(L"{\"schema_version\":\"worker-graph-static-neighbor-read-profile-0.1\"") +
            L",\"profile_id\":" + JsonString(validation.profileId) +
            L",\"target_node_id\":" + JsonString(validation.targetNodeId) +
            L",\"required_neighbor_roles\":" + LoopStringArrayJson(validation.requiredNeighborRoles) +
            L",\"neighbor_count\":" + std::to_wstring(validation.neighborCount) +
            L",\"max_neighbors\":" + std::to_wstring(validation.maxNeighbors) +
            L",\"admitted\":true}";
        validation.present = true;
        return validation;
    }
}
