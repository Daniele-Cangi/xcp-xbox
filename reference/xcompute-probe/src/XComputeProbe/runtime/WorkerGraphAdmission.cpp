#include "pch.h"
#include "WorkerGraphAdmission.h"

#include "WorkerGraphRuntime.h"
#include "../ProbeResult.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        bool IsAllowlistedCommand(std::wstring const& command)
        {
            return command == L"run_artifact_manifest_compute_job" ||
                command == L"reduce_graph_verdict" ||
                command == L"xvm_program" ||
                command == L"gpu_spmd_precompiled" ||
                command == L"control_select" ||
                command == L"neighbor_read";
        }

        void ValidateSafeArtifactId(std::wstring const& artifactId)
        {
            if (!WorkerGraphIsSafeId(artifactId))
            {
                throw WorkerGraphValidationError(
                    "submit_graph.result_artifact_id_invalid",
                    "result_artifact_id must be a safe worker artifact id");
            }
        }

        void ValidateControlSelectShape(
            WorkerGraphAdmittedNode const& admitted,
            std::vector<WorkerGraphAdmittedEdge> const& edges)
        {
            if (edges.size() < 3)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_edges_required",
                    "control_select requires selector, on_pass and on_fail edges");
            }

            auto passRole = WorkerGraphOptionalString(admitted.node, L"on_pass_role", L"on_pass");
            auto failRole = WorkerGraphOptionalString(admitted.node, L"on_fail_role", L"on_fail");
            if (!WorkerGraphIsSafeId(passRole) || !WorkerGraphIsSafeId(failRole) || passRole == failRole)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_role_invalid",
                    "control_select branch roles must be safe and distinct");
            }

            uint64_t selectorCount = 0;
            uint64_t passCount = 0;
            uint64_t failCount = 0;
            for (auto const& edge : edges)
            {
                if (edge.edgeType == L"control" && edge.role == L"selector")
                {
                    ++selectorCount;
                }
                else if (edge.edgeType == L"artifact" && edge.role == passRole)
                {
                    ++passCount;
                }
                else if (edge.edgeType == L"artifact" && edge.role == failRole)
                {
                    ++failCount;
                }
            }
            if (selectorCount == 0)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_selector_required",
                    "control_select requires one control edge with role selector");
            }
            if (selectorCount > 1)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_selector_duplicate",
                    "control_select selector edge is duplicated");
            }
            if (passCount == 0 || failCount == 0)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_branch_missing",
                    "control_select requires declared on_pass and on_fail artifact branches");
            }
            if (passCount > 1 || failCount > 1)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.control_select_branch_duplicate",
                    "control_select branch edge is duplicated");
            }
        }

        void ValidateNodeCapability(
            WorkerGraphAdmittedNode& admitted,
            WorkerGraphStaticNeighborReadValidation const& staticNeighborRead,
            WorkerXvmProgramResolver const& xvmResolver,
            WorkerXvmProgramTextReader const& xvmTextReader,
            WorkerXvmSnapshotAuthorityProvider const& xvmSnapshotAuthorityLoader)
        {
            auto const& node = admitted.node;
            auto const& command = admitted.command;
            auto const& binding = admitted.inputBindingMode;
            if (!IsAllowlistedCommand(command))
            {
                throw WorkerGraphValidationError(
                    "submit_graph.command_not_allowlisted",
                    "submit_graph node command is not an admitted worker capability");
            }

            if (command == L"run_artifact_manifest_compute_job")
            {
                if (WorkerGraphOptionalString(node, L"kernel_id").empty())
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.kernel_id_required",
                        "graph compute nodes must provide kernel_id explicitly");
                }
                auto manifestArtifactId = WorkerGraphOptionalString(node, L"manifest_artifact_id");
                if (!WorkerGraphIsSafeId(manifestArtifactId))
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.manifest_artifact_id_required",
                        "graph compute nodes must provide a safe manifest_artifact_id explicitly");
                }
                return;
            }

            if (command == L"reduce_graph_verdict")
            {
                if (WorkerGraphOptionalString(node, L"kernel_id", L"graph_reduce_verdict_v1") != L"graph_reduce_verdict_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.reduce_kernel_id_invalid",
                        "reduce_graph_verdict requires kernel_id graph_reduce_verdict_v1");
                }
                if (admitted.inputEdges.empty())
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.reduce_input_edges_required",
                        "reduce_graph_verdict requires at least one resolved input edge");
                }
                if (binding != L"mix_bound_input_edges_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.input_binding_mode_invalid",
                        "reduce_graph_verdict requires input_binding mode mix_bound_input_edges_v1");
                }
                return;
            }

            if (command == L"gpu_spmd_precompiled")
            {
                if (WorkerGraphOptionalString(node, L"kernel_id", L"matrix_block_fp32_v1") != L"matrix_block_fp32_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.gpu_kernel_id_invalid",
                        "gpu_spmd_precompiled requires kernel_id matrix_block_fp32_v1");
                }
                if (WorkerGraphOptionalString(node, L"backend", L"d3d12_precompiled_shader_shape") != L"d3d12_precompiled_shader_shape")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.gpu_backend_invalid",
                        "gpu_spmd_precompiled admits only d3d12_precompiled_shader_shape");
                }
                if (!binding.empty() && binding != L"mix_bound_input_edges_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.gpu_input_binding_invalid",
                        "gpu_spmd_precompiled accepts only mix_bound_input_edges_v1 input binding");
                }
                (void)WorkerGraphReadNodeResourceLimits(node, true);
                return;
            }

            if (command == L"neighbor_read")
            {
                if (!staticNeighborRead.present)
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.neighbor_profile_required",
                        "neighbor_read requires a declared static_neighbor_read_profile");
                }
                if (WorkerGraphOptionalString(node, L"kernel_id", L"static_neighbor_read_v1") != L"static_neighbor_read_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.neighbor_kernel_id_invalid",
                        "neighbor_read requires kernel_id static_neighbor_read_v1");
                }
                if (binding != L"static_neighbor_read_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.input_binding_mode_invalid",
                        "neighbor_read requires input_binding mode static_neighbor_read_v1");
                }
                if (admitted.inputEdges.empty() || admitted.inputEdges.size() > WorkerGraphMaxStaticNeighborReadEdges())
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.neighbor_count_invalid",
                        "neighbor_read input edge count is outside the admitted range");
                }
                return;
            }

            if (command == L"control_select")
            {
                if (WorkerGraphOptionalString(node, L"kernel_id", L"control_select_v1") != L"control_select_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.control_select_kernel_id_invalid",
                        "control_select requires kernel_id control_select_v1");
                }
                if (binding != L"control_select_v1")
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.input_binding_mode_invalid",
                        "control_select requires input_binding mode control_select_v1");
                }
                ValidateControlSelectShape(admitted, admitted.inputEdges);
                return;
            }

            if (binding != L"typed_xvm_inputs_v1")
            {
                throw WorkerGraphValidationError(
                    "submit_graph.xvm_input_binding_invalid",
                    "xvm_program requires input_binding mode typed_xvm_inputs_v1");
            }
            // WorkerValidateXvmProgramAdmission remains the compatibility wrapper;
            // the detailed path also binds the typed GPU execution plan.
            auto preAdmission = WorkerPreAdmitXvmProgram(
                node,
                xvmResolver,
                xvmTextReader,
                xvmSnapshotAuthorityLoader);
            admitted.xvmStaticFuelProofJson = std::move(preAdmission.staticFuelProofJson);
            admitted.xvmGpuExecutionPlanJson = std::move(preAdmission.gpuExecutionPlanJson);
            admitted.xvmGpuExecutionPlan = std::move(preAdmission.gpuExecutionPlan);
            admitted.xvmSpmdExecutionPlanJson = std::move(preAdmission.spmdExecutionPlanJson);
            admitted.xvmSpmdExecutionPlan = std::move(preAdmission.spmdExecutionPlan);
            admitted.xvmCpuExecutionPlanJson = std::move(preAdmission.cpuExecutionPlanJson);
            admitted.xvmCpuExecutionPlan = std::move(preAdmission.cpuExecutionPlan);
            admitted.xvmCpuCapsulePlanJson = std::move(preAdmission.cpuCapsulePlanJson);
            admitted.xvmCpuCapsulePlan = std::move(preAdmission.cpuCapsulePlan);
            admitted.xvmBackendConvergencePlanJson =
                std::move(preAdmission.backendConvergencePlanJson);
            admitted.xvmBackendConvergencePlan =
                std::move(preAdmission.backendConvergencePlan);
            admitted.xvmProductionModePlanJson =
                std::move(preAdmission.productionModePlanJson);
            admitted.xvmProductionModePlan =
                std::move(preAdmission.productionModePlan);
            admitted.xvmCpuPlanBuildElapsedMs = preAdmission.cpuPlanBuildElapsedMs;
        }

        std::wstring EdgePlanJson(WorkerGraphAdmittedEdge const& edge)
        {
            return L"{\"source_node_index\":" + std::to_wstring(edge.sourceNodeIndex) +
                L",\"from_node_id\":" + JsonString(edge.fromNodeId) +
                L",\"role\":" + JsonString(edge.role) +
                L",\"edge_type\":" + JsonString(edge.edgeType) +
                L",\"value_type\":" + JsonString(edge.valueType) +
                L",\"expected_result_artifact_id\":" + JsonString(edge.expectedArtifactId) + L"}";
        }
    }

    WorkerGraphExecutionPlan WorkerGraphAdmitExecutionPlan(
        JsonObject const& request,
        std::wstring const& generatedGraphId,
        WorkerXvmProgramResolver const& xvmResolver,
        WorkerXvmProgramTextReader const& xvmTextReader,
        WorkerXvmSnapshotAuthorityProvider const& xvmSnapshotAuthorityLoader)
    {
        auto requestValidation = WorkerGraphValidateRequest(
            request,
            generatedGraphId,
            WorkerMaxOnDeviceGraphNodes());
        auto expansion = WorkerGraphApplyBoundedStaticExpansion(
            requestValidation.graph,
            requestValidation.nodes);

        WorkerGraphExecutionPlan plan;
        plan.graph = expansion.graph;
        plan.graphId = requestValidation.graphId;
        plan.graphSchemaVersion = WorkerGraphOptionalString(plan.graph, L"schema_version");
        plan.expansion = expansion;

        auto nodes = expansion.nodes;
        plan.nodes.reserve(nodes.Size());
        std::map<std::wstring, uint32_t> nodeIndexes;
        std::set<std::wstring> resultArtifactIds;
        for (uint32_t nodeIndex = 0; nodeIndex < nodes.Size(); ++nodeIndex)
        {
            auto validation = WorkerGraphValidateNodeValue(nodes.GetAt(nodeIndex), nodeIndex, plan.graphId);
            WorkerGraphValidateUniqueNodeId(nodeIndexes.find(validation.nodeId) != nodeIndexes.end());
            ValidateSafeArtifactId(validation.resultArtifactId);
            if (!resultArtifactIds.insert(validation.resultArtifactId).second)
            {
                throw WorkerGraphValidationError(
                    "submit_graph.result_artifact_id_duplicate",
                    "result_artifact_id values must be unique within an admitted graph");
            }

            WorkerGraphAdmittedNode admitted;
            admitted.node = validation.node;
            admitted.nodeId = validation.nodeId;
            admitted.command = WorkerGraphOptionalString(validation.node, L"command");
            admitted.resultArtifactId = validation.resultArtifactId;
            nodeIndexes.emplace(admitted.nodeId, nodeIndex);
            plan.nodes.push_back(std::move(admitted));
        }

        plan.resourceLedger = WorkerGraphAdmitResourceLedger(plan.graph, nodes);
        plan.boundedLoop = WorkerGraphValidateBoundedLoopProfile(plan.graph, nodes);
        plan.staticNeighborRead = WorkerGraphValidateStaticNeighborReadProfile(plan.graph, nodes);
        if (plan.boundedLoop.present && plan.boundedLoop.fuelBudget > plan.resourceLedger.fuelLimit)
        {
            throw WorkerGraphValidationError(
                "submit_graph.loop_fuel_exceeded",
                "bounded loop fuel budget exceeds graph resource policy");
        }

        uint64_t edgeCount = 0;
        for (uint32_t nodeIndex = 0; nodeIndex < plan.nodes.size(); ++nodeIndex)
        {
            auto& admitted = plan.nodes[nodeIndex];
            std::vector<std::wstring> inputEdgeKeys;
            auto edgeArray = WorkerGraphValidateInputEdgesArray(admitted.node);
            if (edgeArray.present)
            {
                for (uint32_t edgeIndex = 0; edgeIndex < edgeArray.edges.Size(); ++edgeIndex)
                {
                    auto edge = WorkerGraphValidateInputEdgeSource(
                        edgeArray.edges.GetAt(edgeIndex),
                        edgeCount,
                        WorkerMaxOnDeviceGraphEdges());
                    auto source = nodeIndexes.find(edge.fromNodeId);
                    if (source == nodeIndexes.end() || source->second >= nodeIndex)
                    {
                        throw WorkerGraphValidationError(
                            "submit_graph.edge_source_unresolved",
                            "input edge must reference a prior node in the immutable execution plan");
                    }
                    auto const& sourceNode = plan.nodes[source->second];
                    if (!edge.expectedArtifactId.empty() && edge.expectedArtifactId != sourceNode.resultArtifactId)
                    {
                        throw WorkerGraphValidationError(
                            "submit_graph.edge_artifact_mismatch",
                            "input edge expected_result_artifact_id does not match the admitted source result artifact");
                    }
                    WorkerGraphValidateInputEdgeRole(edge, inputEdgeKeys);
                    inputEdgeKeys.push_back(edge.edgeKey);

                    WorkerGraphAdmittedEdge admittedEdge;
                    admittedEdge.sourceNodeIndex = source->second;
                    admittedEdge.fromNodeId = edge.fromNodeId;
                    admittedEdge.role = edge.role;
                    admittedEdge.edgeType = edge.edgeType;
                    admittedEdge.valueType = edge.valueType;
                    admittedEdge.expectedArtifactId = edge.expectedArtifactId;
                    admitted.inputEdges.push_back(std::move(admittedEdge));
                    ++edgeCount;
                }
            }
            admitted.inputBindingMode = WorkerGraphValidateInputBindingMode(
                admitted.node,
                !admitted.inputEdges.empty());
            ValidateNodeCapability(
                admitted,
                plan.staticNeighborRead,
                xvmResolver,
                xvmTextReader,
                xvmSnapshotAuthorityLoader);
        }
        plan.edgeCount = edgeCount;
        return plan;
    }

    std::wstring WorkerGraphExecutionPlanJson(WorkerGraphExecutionPlan const& plan)
    {
        std::wostringstream nodes;
        nodes << L"[";
        for (size_t nodeIndex = 0; nodeIndex < plan.nodes.size(); ++nodeIndex)
        {
            if (nodeIndex != 0)
            {
                nodes << L",";
            }
            auto const& node = plan.nodes[nodeIndex];
            nodes << L"{\"node_index\":" << nodeIndex
                << L",\"node_id\":" << JsonString(node.nodeId)
                << L",\"command\":" << JsonString(node.command)
                << L",\"result_artifact_id\":" << JsonString(node.resultArtifactId)
                << L",\"input_binding_mode\":" << JsonString(node.inputBindingMode)
                << L",\"xvm_static_fuel_proof\":" << node.xvmStaticFuelProofJson
                << L",\"xvm_gpu_execution_plan\":" << node.xvmGpuExecutionPlanJson
                << L",\"xvm_spmd_execution_plan\":" << node.xvmSpmdExecutionPlanJson
                << L",\"xvm_cpu_execution_plan\":" << node.xvmCpuExecutionPlanJson
                << L",\"xvm_cpu_capsule_plan\":" << node.xvmCpuCapsulePlanJson
                << L",\"xvm_backend_convergence_plan\":" <<
                    node.xvmBackendConvergencePlanJson
                << L",\"xvm_production_mode_plan\":" <<
                    node.xvmProductionModePlanJson
                << L",\"input_edges\":[";
            for (size_t edgeIndex = 0; edgeIndex < node.inputEdges.size(); ++edgeIndex)
            {
                if (edgeIndex != 0)
                {
                    nodes << L",";
                }
                nodes << EdgePlanJson(node.inputEdges[edgeIndex]);
            }
            nodes << L"]}";
        }
        nodes << L"]";

        return std::wstring(L"{\"schema_version\":") + JsonString(WorkerGraphExecutionPlanSchemaVersion) +
            L",\"graph_id\":" + JsonString(plan.graphId) +
            L",\"graph_schema_version\":" + JsonString(plan.graphSchemaVersion) +
            L",\"node_count\":" + std::to_wstring(plan.nodes.size()) +
            L",\"edge_count\":" + std::to_wstring(plan.edgeCount) +
            L",\"fully_materialized\":true" +
            L",\"side_effect_free_admission\":true" +
            L",\"immutable_after_admission\":true" +
            L",\"resource_ledger\":" + WorkerGraphResourceLedgerJson(plan.resourceLedger) +
            L",\"nodes\":" + nodes.str() + L"}";
    }
}
