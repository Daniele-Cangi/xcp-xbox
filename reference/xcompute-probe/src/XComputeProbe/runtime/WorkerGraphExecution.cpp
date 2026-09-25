#include "pch.h"
#include "WorkerGraphExecution.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphValidation.h"
#include "WorkerCanonicalResultTelemetryRuntime.h"
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

        std::wstring UInt64HexLocal(uint64_t value)
        {
            std::wostringstream out;
            out << std::hex << std::setfill(L'0') << std::setw(16) << value;
            return out.str();
        }

        std::wstring BoolJsonLocal(bool value)
        {
            return value ? L"true" : L"false";
        }

        void InsertString(JsonObject& object, wchar_t const* name, std::wstring const& value)
        {
            object.Insert(name, JsonValue::CreateStringValue(hstring(value)));
        }

        void InsertNumber(JsonObject& object, wchar_t const* name, uint64_t value)
        {
            object.Insert(name, JsonValue::CreateNumberValue(static_cast<double>(value)));
        }

        uint64_t OptionalUInt64(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedNumber(name, static_cast<double>(fallback));
            if (value < 0)
            {
                throw WorkerGraphExecutionError("argument.invalid_number", "numeric argument cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }

        uint64_t OptionalUInt64NoThrow(JsonObject const& object, wchar_t const* name, uint64_t fallback = 0)
        {
            try
            {
                if (!object.HasKey(name))
                {
                    return fallback;
                }
                auto value = object.GetNamedNumber(name, static_cast<double>(fallback));
                if (value < 0)
                {
                    return fallback;
                }
                return static_cast<uint64_t>(value);
            }
            catch (...)
            {
                return fallback;
            }
        }

        void FinalizeNodeResult(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphNodeResultSummary& nodeSummary,
            WorkerGraphNodeExecutionResult& execution)
        {
            auto boundary = WorkerFinalizeGraphNodeResultBoundary(input.protocolVersion, nodeSummary);
            nodeSummary.functionalCanonicalJson = boundary.functionalCanonicalJson;
            nodeSummary.resultEnvelopeJson = boundary.resultEnvelopeJson;
            execution.nodeResultJson = boundary.observedNodeResultJson;
            execution.canonicalNodeResultJson = boundary.canonicalNodeResultJson;
        }

        WorkerGraphNodeExecutionResult RunComputeGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphComputeNodeRunner const& computeRunner)
        {
            auto kernelId = WorkerGraphOptionalString(input.node, L"kernel_id");
            if (kernelId.empty())
            {
                throw WorkerGraphExecutionError("submit_graph.kernel_id_required", "graph compute nodes must provide kernel_id explicitly");
            }
            auto manifestArtifactId = WorkerGraphOptionalString(input.node, L"manifest_artifact_id");
            if (manifestArtifactId.empty())
            {
                throw WorkerGraphExecutionError("submit_graph.manifest_artifact_id_required", "graph compute nodes must provide manifest_artifact_id explicitly");
            }

            JsonObject nodeRequest;
            InsertString(nodeRequest, L"command", L"run_artifact_manifest_compute_job");
            InsertString(nodeRequest, L"kernel_id", kernelId);
            InsertString(nodeRequest, L"manifest_artifact_id", manifestArtifactId);
            InsertString(nodeRequest, L"artifact_id", input.resultArtifactId);
            InsertString(nodeRequest, L"artifact_kind", WorkerGraphOptionalString(input.node, L"artifact_kind", L"on-device-graph-node-result"));
            if (input.node.HasKey(L"expected_logical_sha256"))
            {
                InsertString(nodeRequest, L"expected_logical_sha256", WorkerGraphOptionalString(input.node, L"expected_logical_sha256"));
            }
            if (input.node.HasKey(L"tile_bytes"))
            {
                InsertNumber(nodeRequest, L"tile_bytes", OptionalUInt64(input.node, L"tile_bytes"));
            }
            if (input.node.HasKey(L"rollback_headroom_bytes"))
            {
                InsertNumber(nodeRequest, L"rollback_headroom_bytes", OptionalUInt64(input.node, L"rollback_headroom_bytes"));
            }
            if (input.node.HasKey(L"workspace_budget_bytes"))
            {
                InsertNumber(nodeRequest, L"workspace_budget_bytes", OptionalUInt64(input.node, L"workspace_budget_bytes"));
            }
            if (!input.boundInputSha256.empty())
            {
                InsertString(nodeRequest, L"graph_bound_input_sha256", input.boundInputSha256);
                InsertString(nodeRequest, L"graph_bound_input_mix64", input.boundInputMix64);
            }

            auto rawResult = computeRunner(nodeRequest);
            auto result = JsonObject::Parse(rawResult);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = result.GetNamedBoolean(L"ok", false);
            execution.verified = result.GetNamedBoolean(L"verified", false);
            execution.logicalBytesRead = OptionalUInt64NoThrow(result, L"logical_bytes_read", 0);

            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = WorkerGraphOptionalString(result, L"result_artifact_id", input.resultArtifactId);
            execution.record.resultSha256 = WorkerGraphOptionalString(result, L"result_sha256");
            execution.record.logicalSha256 = WorkerGraphOptionalString(result, L"logical_sha256");
            execution.record.computeMix64 = WorkerGraphOptionalString(result, L"compute_mix64");
            execution.record.kernelId = WorkerGraphOptionalString(result, L"kernel_id", kernelId);
            execution.record.controlToken = execution.ok && execution.verified ? L"pass" : L"fail";
            execution.record.ok = execution.ok;

            WorkerGraphNodeResultSummary nodeSummary;
            nodeSummary.nodeId = input.nodeId;
            nodeSummary.command = input.command;
            nodeSummary.kernelId = execution.record.kernelId;
            nodeSummary.ok = execution.ok;
            nodeSummary.computeKind = WorkerGraphOptionalString(result, L"compute_kind");
            nodeSummary.manifestArtifactId = manifestArtifactId;
            nodeSummary.resultArtifactId = execution.record.resultArtifactId;
            nodeSummary.resultSha256 = execution.record.resultSha256;
            nodeSummary.logicalSha256 = execution.record.logicalSha256;
            nodeSummary.logicalBytesRead = execution.logicalBytesRead;
            nodeSummary.computeMix64 = execution.record.computeMix64;
            nodeSummary.controlToken = execution.record.controlToken;
            nodeSummary.inputBindingMode = input.inputBindingMode;
            nodeSummary.boundInputSha256 = WorkerGraphOptionalString(result, L"graph_bound_input_sha256");
            nodeSummary.boundInputMix64 = WorkerGraphOptionalString(result, L"graph_bound_input_mix64");
            nodeSummary.boundInputApplied = result.GetNamedBoolean(L"graph_bound_input_applied", false);
            nodeSummary.verified = execution.verified;
            nodeSummary.inputEdgeCount = input.inputEdgeCount;
            nodeSummary.inputEdgesJson = input.inputEdgesJson;
            nodeSummary.resourceUsageJson = WorkerGraphResourceUsageJson(0, 0, 0);
            FinalizeNodeResult(input, nodeSummary, execution);
            return execution;
        }

        WorkerGraphNodeExecutionResult RunGpuSpmdGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphComputeNodeRunner const& computeRunner,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphWideMix64 const& wideMix64)
        {
            auto kernelId = WorkerGraphOptionalString(input.node, L"kernel_id", L"matrix_block_fp32_v1");
            if (kernelId != L"matrix_block_fp32_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.gpu_kernel_id_invalid", "gpu_spmd_precompiled requires kernel_id matrix_block_fp32_v1");
            }
            auto backend = WorkerGraphOptionalString(input.node, L"backend", L"d3d12_precompiled_shader_shape");
            if (backend != L"d3d12_precompiled_shader_shape")
            {
                throw WorkerGraphExecutionError("submit_graph.gpu_backend_invalid", "gpu_spmd_precompiled admits only d3d12_precompiled_shader_shape");
            }
            if (!input.inputBindingMode.empty() && input.inputBindingMode != L"mix_bound_input_edges_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.gpu_input_binding_invalid", "gpu_spmd_precompiled accepts only mix_bound_input_edges_v1 input binding");
            }

            auto limits = WorkerGraphReadNodeResourceLimits(input.node, true);
            JsonObject nodeRequest;
            InsertString(nodeRequest, L"command", L"run_d3d12_shader_shape_job");
            InsertString(nodeRequest, L"kernel_id", kernelId);
            if (input.node.HasKey(L"elements_list"))
            {
                InsertString(nodeRequest, L"elements_list", WorkerGraphOptionalString(input.node, L"elements_list"));
            }
            if (input.node.HasKey(L"variants"))
            {
                InsertString(nodeRequest, L"variants", WorkerGraphOptionalString(input.node, L"variants"));
            }
            if (input.node.HasKey(L"repeats"))
            {
                InsertNumber(nodeRequest, L"repeats", OptionalUInt64(input.node, L"repeats"));
            }
            if (input.node.HasKey(L"warmup_repeats"))
            {
                InsertNumber(nodeRequest, L"warmup_repeats", OptionalUInt64(input.node, L"warmup_repeats"));
            }

            auto rawResult = computeRunner(nodeRequest);
            auto result = JsonObject::Parse(rawResult);
            auto resultCount = OptionalUInt64NoThrow(result, L"result_count", 0);
            if (resultCount > limits.fuel)
            {
                throw WorkerGraphExecutionError("submit_graph.gpu_fuel_exhausted", "gpu_spmd_precompiled result count exceeded admitted node fuel");
            }

            auto rawResultUtf8 = WideToUtf8Local(rawResult);
            auto outputBytes = static_cast<uint64_t>(rawResultUtf8.size());
            if (outputBytes > limits.outputBytes)
            {
                throw WorkerGraphExecutionError("submit_graph.gpu_output_exhausted", "gpu_spmd_precompiled result payload exceeded admitted output bytes");
            }

            auto rawVerified = result.GetNamedBoolean(L"verified", false);
            auto dynamicCode = result.GetNamedBoolean(L"dynamic_code", true);
            auto verified = rawVerified && !dynamicCode && resultCount > 0;
            auto aggregateHash32 = OptionalUInt64NoThrow(result, L"aggregate_hash32", 0);
            auto maxMismatchCount = OptionalUInt64NoThrow(result, L"max_mismatch_count", 0);
            auto totalGpuTimingSamples = OptionalUInt64NoThrow(result, L"gpu_timing_sample_count", 0);
            auto totalElementsTimed = OptionalUInt64NoThrow(result, L"total_elements_timed", 0);

            auto functionalCanonical = std::wstring(L"{\"schema_version\":\"gpu-spmd-precompiled-functional-result-0.1\"") +
                L",\"profile\":\"d3d12-precompiled-shader-shape-v1\"" +
                L",\"backend\":" + JsonString(backend) +
                L",\"kernel_id\":" + JsonString(kernelId) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"input_edge_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"result_count\":" + std::to_wstring(resultCount) +
                L",\"gpu_timing_sample_count\":" + std::to_wstring(totalGpuTimingSamples) +
                L",\"total_elements_timed\":" + std::to_wstring(totalElementsTimed) +
                L",\"aggregate_hash32\":" + std::to_wstring(aggregateHash32) +
                L",\"max_mismatch_count\":" + std::to_wstring(maxMismatchCount) +
                L",\"dynamic_code\":false" +
                L",\"runtime_shader_compilation_used\":false" +
                L",\"verified\":" + BoolJsonLocal(verified) +
                L"}";
            auto logicalSha256 = sha256Text(WideToUtf8Local(functionalCanonical));
            auto computeMix64 = UInt64HexLocal(wideMix64(functionalCanonical, 0x452821e638d01377ull));
            auto resourceUsageJson = WorkerGraphResourceUsageJson(resultCount, limits.memoryBytes, outputBytes);
            auto payload = std::wstring(L"{\"schema_version\":\"gpu-spmd-precompiled-result-0.1\"") +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"node_id\":" + JsonString(input.nodeId) +
                L",\"command\":\"gpu_spmd_precompiled\"" +
                L",\"compute_kind\":\"d3d12_precompiled_gpu_spmd_v1\"" +
                L",\"backend\":" + JsonString(backend) +
                L",\"kernel_id\":" + JsonString(kernelId) +
                L",\"functional_canonical\":" + functionalCanonical +
                L",\"logical_sha256\":" + JsonString(logicalSha256) +
                L",\"compute_mix64\":" + JsonString(computeMix64) +
                L",\"resource_usage\":" + resourceUsageJson +
                L",\"input_edges\":" + input.inputEdgesJson +
                L",\"bound_input_applied_to_canonical\":" + BoolJsonLocal(!input.boundInputSha256.empty()) +
                L",\"bound_input_applied_to_shader\":false" +
                L",\"d3d12_result\":" + rawResult +
                L"}";

            JsonObject publishRequest;
            InsertString(publishRequest, L"artifact_id", input.resultArtifactId);
            InsertString(publishRequest, L"artifact_kind", WorkerGraphOptionalString(input.node, L"artifact_kind", L"gpu-spmd-precompiled-result-v1"));
            if (input.node.HasKey(L"rollback_headroom_bytes"))
            {
                InsertNumber(publishRequest, L"rollback_headroom_bytes", OptionalUInt64(input.node, L"rollback_headroom_bytes"));
            }
            if (input.node.HasKey(L"workspace_budget_bytes"))
            {
                InsertNumber(publishRequest, L"workspace_budget_bytes", OptionalUInt64(input.node, L"workspace_budget_bytes"));
            }
            auto manifestFields = std::wstring(L",\"job_output_schema\":\"gpu-spmd-precompiled-result-0.1\"") +
                L",\"kernel_id\":" + JsonString(kernelId) +
                L",\"compute_kind\":\"d3d12_precompiled_gpu_spmd_v1\"" +
                L",\"logical_sha256\":" + JsonString(logicalSha256) +
                L",\"compute_mix64\":" + JsonString(computeMix64) +
                L",\"result_count\":" + std::to_wstring(resultCount);
            auto published = artifactPublisher(
                publishRequest,
                input.resultArtifactId,
                L"gpu-spmd-precompiled-result-v1",
                WideToUtf8Local(payload),
                manifestFields);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = result.GetNamedBoolean(L"ok", false);
            execution.verified = verified;
            execution.logicalBytesRead = totalElementsTimed;
            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = published.artifactId;
            execution.record.resultSha256 = published.sha256;
            execution.record.logicalSha256 = logicalSha256;
            execution.record.computeMix64 = computeMix64;
            execution.record.kernelId = kernelId;
            execution.record.controlToken = execution.ok && execution.verified ? L"pass" : L"fail";
            execution.record.fuelConsumed = resultCount;
            execution.record.memoryBytes = limits.memoryBytes;
            execution.record.outputBytes = outputBytes;
            execution.record.ok = execution.ok;

            WorkerGraphNodeResultSummary summary;
            summary.nodeId = input.nodeId;
            summary.command = input.command;
            summary.kernelId = kernelId;
            summary.computeKind = L"d3d12_precompiled_gpu_spmd_v1";
            summary.resultArtifactId = published.artifactId;
            summary.resultSha256 = published.sha256;
            summary.logicalSha256 = logicalSha256;
            summary.logicalBytesRead = execution.logicalBytesRead;
            summary.computeMix64 = computeMix64;
            summary.controlToken = execution.record.controlToken;
            summary.inputBindingMode = input.inputBindingMode;
            summary.boundInputSha256 = input.boundInputSha256;
            summary.boundInputMix64 = input.boundInputMix64;
            summary.boundInputApplied = !input.boundInputSha256.empty();
            summary.verified = execution.verified;
            summary.ok = execution.ok;
            summary.inputEdgeCount = input.inputEdgeCount;
            summary.inputEdgesJson = input.inputEdgesJson;
            summary.fuelConsumed = execution.record.fuelConsumed;
            summary.memoryBytes = execution.record.memoryBytes;
            summary.outputBytes = execution.record.outputBytes;
            summary.resourceUsageJson = resourceUsageJson;
            FinalizeNodeResult(input, summary, execution);
            return execution;
        }

        WorkerGraphNodeExecutionResult RunReduceGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphWideMix64 const& wideMix64)
        {
            auto kernelId = WorkerGraphOptionalString(input.node, L"kernel_id", L"graph_reduce_verdict_v1");
            if (kernelId != L"graph_reduce_verdict_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.reduce_kernel_id_invalid", "reduce_graph_verdict requires kernel_id graph_reduce_verdict_v1");
            }
            if (input.inputEdgeCount == 0)
            {
                throw WorkerGraphExecutionError("submit_graph.reduce_input_edges_required", "reduce_graph_verdict requires at least one resolved input edge");
            }
            if (input.inputBindingMode != L"mix_bound_input_edges_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.input_binding_mode_invalid", "reduce_graph_verdict requires input_binding mode mix_bound_input_edges_v1");
            }

            std::wstring reduceVerdict = input.inputEdgesAllOk ? L"PASS" : L"FAIL";
            auto reduceSummary =
                std::wstring(L"{\"kernel_id\":\"graph_reduce_verdict_v1\"") +
                L",\"compute_kind\":\"canonical_graph_reduce_v1\"" +
                L",\"input_edge_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"verdict\":" + JsonString(reduceVerdict) +
                L",\"deterministic_input_edges\":" + input.deterministicInputEdgesJson +
                L"}";
            auto reduceLogicalSha256 = sha256Text(WideToUtf8Local(reduceSummary));
            auto reduceComputeMix64 = UInt64HexLocal(wideMix64(reduceSummary, 0xa4093822299f31d0ull));
            auto resultPayload =
                std::wstring(L"{\"schema_version\":\"worker-on-device-graph-reduce-result-0.1\"") +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"node_id\":" + JsonString(input.nodeId) +
                L",\"kernel_id\":\"graph_reduce_verdict_v1\"" +
                L",\"compute_kind\":\"canonical_graph_reduce_v1\"" +
                L",\"input_binding_mode\":\"mix_bound_input_edges_v1\"" +
                L",\"input_edge_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"reduce_logical_sha256\":" + JsonString(reduceLogicalSha256) +
                L",\"compute_mix64\":" + JsonString(reduceComputeMix64) +
                L",\"verdict\":" + JsonString(reduceVerdict) +
                L",\"verified\":" + BoolJsonLocal(input.inputEdgesAllOk) +
                L",\"source_ok_all\":" + BoolJsonLocal(input.inputEdgesAllOk) +
                L",\"deterministic_summary_sha256\":" + JsonString(reduceLogicalSha256) +
                L",\"deterministic_input_edges\":" + input.deterministicInputEdgesJson +
                L"}";

            JsonObject publishRequest;
            InsertString(publishRequest, L"artifact_id", input.resultArtifactId);
            InsertString(publishRequest, L"artifact_kind", WorkerGraphOptionalString(input.node, L"artifact_kind", L"on-device-graph-reduce-result"));
            if (input.node.HasKey(L"rollback_headroom_bytes"))
            {
                InsertNumber(publishRequest, L"rollback_headroom_bytes", OptionalUInt64(input.node, L"rollback_headroom_bytes"));
            }
            if (input.node.HasKey(L"workspace_budget_bytes"))
            {
                InsertNumber(publishRequest, L"workspace_budget_bytes", OptionalUInt64(input.node, L"workspace_budget_bytes"));
            }
            auto manifestFields =
                std::wstring(L",\"job_output_schema\":\"worker-on-device-graph-reduce-result-0.1\"") +
                L",\"kernel_id\":\"graph_reduce_verdict_v1\"" +
                L",\"compute_kind\":\"canonical_graph_reduce_v1\"" +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"compute_mix64\":" + JsonString(reduceComputeMix64) +
                L",\"verdict\":" + JsonString(reduceVerdict);
            auto published = artifactPublisher(
                publishRequest,
                input.resultArtifactId,
                L"on-device-graph-reduce-result",
                WideToUtf8Local(resultPayload),
                manifestFields);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = true;
            execution.verified = input.inputEdgesAllOk;
            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = published.artifactId;
            execution.record.resultSha256 = published.sha256;
            execution.record.logicalSha256 = reduceLogicalSha256;
            execution.record.computeMix64 = reduceComputeMix64;
            execution.record.kernelId = kernelId;
            execution.record.controlToken = reduceVerdict == L"PASS" ? L"pass" : L"fail";
            execution.record.ok = execution.ok;

            WorkerGraphNodeResultSummary nodeSummary;
            nodeSummary.nodeId = input.nodeId;
            nodeSummary.command = input.command;
            nodeSummary.kernelId = execution.record.kernelId;
            nodeSummary.ok = execution.ok;
            nodeSummary.computeKind = L"canonical_graph_reduce_v1";
            nodeSummary.resultArtifactId = execution.record.resultArtifactId;
            nodeSummary.resultSha256 = execution.record.resultSha256;
            nodeSummary.logicalSha256 = execution.record.logicalSha256;
            nodeSummary.computeMix64 = execution.record.computeMix64;
            nodeSummary.controlToken = execution.record.controlToken;
            nodeSummary.inputBindingMode = input.inputBindingMode;
            nodeSummary.boundInputSha256 = input.boundInputSha256;
            nodeSummary.boundInputMix64 = input.boundInputMix64;
            nodeSummary.boundInputApplied = true;
            nodeSummary.verified = execution.verified;
            nodeSummary.inputEdgeCount = input.inputEdgeCount;
            nodeSummary.inputEdgesJson = input.inputEdgesJson;
            nodeSummary.resourceUsageJson = WorkerGraphResourceUsageJson(0, 0, 0);
            FinalizeNodeResult(input, nodeSummary, execution);
            return execution;
        }

        struct ControlSelectEdge
        {
            std::wstring fromNodeId;
            std::wstring role;
            std::wstring edgeType;
            std::wstring resultArtifactId;
            std::wstring resultSha256;
            std::wstring logicalSha256;
            std::wstring computeMix64;
            std::wstring controlToken;
            bool sourceOk = false;
            bool present = false;
        };

        ControlSelectEdge ReadControlSelectEdge(JsonObject const& edge)
        {
            ControlSelectEdge parsed;
            parsed.fromNodeId = WorkerGraphOptionalString(edge, L"from_node_id");
            parsed.role = WorkerGraphOptionalString(edge, L"role");
            parsed.edgeType = WorkerGraphOptionalString(edge, L"edge_type");
            parsed.resultArtifactId = WorkerGraphOptionalString(edge, L"result_artifact_id");
            parsed.resultSha256 = WorkerGraphOptionalString(edge, L"result_sha256");
            parsed.logicalSha256 = WorkerGraphOptionalString(edge, L"logical_sha256");
            parsed.computeMix64 = WorkerGraphOptionalString(edge, L"compute_mix64");
            parsed.controlToken = WorkerGraphOptionalString(edge, L"control_token");
            parsed.sourceOk = edge.GetNamedBoolean(L"source_ok", false);
            parsed.present = true;
            return parsed;
        }

        void AssignUniqueControlSelectEdge(
            ControlSelectEdge& target,
            ControlSelectEdge const& candidate,
            char const* duplicateCode)
        {
            if (target.present)
            {
                throw WorkerGraphExecutionError(duplicateCode, "control_select branch or selector edge is duplicated");
            }
            target = candidate;
        }

        WorkerGraphNodeExecutionResult RunNeighborReadGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphWideMix64 const& wideMix64)
        {
            auto kernelId = WorkerGraphOptionalString(input.node, L"kernel_id", L"static_neighbor_read_v1");
            if (kernelId != L"static_neighbor_read_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.neighbor_kernel_id_invalid", "neighbor_read requires kernel_id static_neighbor_read_v1");
            }
            if (input.inputBindingMode != L"static_neighbor_read_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.input_binding_mode_invalid", "neighbor_read requires input_binding mode static_neighbor_read_v1");
            }
            if (input.inputEdgeCount == 0 || input.inputEdgeCount > WorkerGraphMaxStaticNeighborReadEdges())
            {
                throw WorkerGraphExecutionError("submit_graph.neighbor_count_invalid", "neighbor_read input edge count is outside the admitted range");
            }
            if (!input.inputEdgesAllOk)
            {
                throw WorkerGraphExecutionError("submit_graph.neighbor_source_failed", "neighbor_read requires all neighbor sources to have passed");
            }

            auto edges = JsonArray::Parse(hstring(input.inputEdgesJson));
            std::vector<std::wstring> neighborEdges;
            neighborEdges.reserve(edges.Size());
            for (uint32_t i = 0; i < edges.Size(); ++i)
            {
                auto value = edges.GetAt(i);
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw WorkerGraphExecutionError("submit_graph.neighbor_edge_invalid", "neighbor_read input edge payload is invalid");
                }

                auto parsed = ReadControlSelectEdge(value.GetObject());
                if (parsed.edgeType != L"artifact")
                {
                    throw WorkerGraphExecutionError("submit_graph.neighbor_edge_invalid", "neighbor_read accepts artifact neighbor edges only");
                }
                if (!parsed.sourceOk)
                {
                    throw WorkerGraphExecutionError("submit_graph.neighbor_source_failed", "neighbor_read requires all neighbor sources to have passed");
                }
                neighborEdges.push_back(
                    std::wstring(L"{\"role\":") + JsonString(parsed.role) +
                    L",\"from_node_id\":" + JsonString(parsed.fromNodeId) +
                    L",\"result_artifact_id\":" + JsonString(parsed.resultArtifactId) +
                    L",\"result_sha256\":" + JsonString(parsed.resultSha256) +
                    L",\"logical_sha256\":" + JsonString(parsed.logicalSha256) +
                    L",\"compute_mix64\":" + JsonString(parsed.computeMix64) +
                    L",\"control_token\":" + JsonString(parsed.controlToken) +
                    L"}");
            }
            std::sort(neighborEdges.begin(), neighborEdges.end());
            auto neighborEdgesJson = WorkerGraphJsonArray(neighborEdges);

            auto functionalCanonical =
                std::wstring(L"{\"schema_version\":\"worker-graph-static-neighbor-read-functional-0.1\"") +
                L",\"kernel_id\":\"static_neighbor_read_v1\"" +
                L",\"compute_kind\":\"static_neighbor_read_v1\"" +
                L",\"neighbor_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"deterministic_neighbor_edges\":" + input.deterministicInputEdgesJson +
                L"}";
            auto logicalSha256 = sha256Text(WideToUtf8Local(functionalCanonical));
            auto computeMix64 = UInt64HexLocal(wideMix64(functionalCanonical, 0x6a09e667f3bcc908ull));
            auto resultPayload =
                std::wstring(L"{\"schema_version\":\"worker-on-device-graph-neighbor-read-result-0.1\"") +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"node_id\":" + JsonString(input.nodeId) +
                L",\"kernel_id\":\"static_neighbor_read_v1\"" +
                L",\"compute_kind\":\"static_neighbor_read_v1\"" +
                L",\"input_binding_mode\":\"static_neighbor_read_v1\"" +
                L",\"neighbor_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"neighbor_edges\":" + neighborEdgesJson +
                L",\"deterministic_neighbor_edges\":" + input.deterministicInputEdgesJson +
                L",\"logical_sha256\":" + JsonString(logicalSha256) +
                L",\"compute_mix64\":" + JsonString(computeMix64) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"verified\":true" +
                L"}";

            JsonObject publishRequest;
            InsertString(publishRequest, L"artifact_id", input.resultArtifactId);
            InsertString(publishRequest, L"artifact_kind", WorkerGraphOptionalString(input.node, L"artifact_kind", L"on-device-graph-neighbor-read-result"));
            auto manifestFields =
                std::wstring(L",\"job_output_schema\":\"worker-on-device-graph-neighbor-read-result-0.1\"") +
                L",\"kernel_id\":\"static_neighbor_read_v1\"" +
                L",\"compute_kind\":\"static_neighbor_read_v1\"" +
                L",\"neighbor_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"compute_mix64\":" + JsonString(computeMix64);
            auto published = artifactPublisher(
                publishRequest,
                input.resultArtifactId,
                L"on-device-graph-neighbor-read-result",
                WideToUtf8Local(resultPayload),
                manifestFields);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = true;
            execution.verified = true;
            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = published.artifactId;
            execution.record.resultSha256 = published.sha256;
            execution.record.logicalSha256 = logicalSha256;
            execution.record.computeMix64 = computeMix64;
            execution.record.kernelId = kernelId;
            execution.record.controlToken = L"pass";
            execution.record.ok = execution.ok;

            WorkerGraphNodeResultSummary nodeSummary;
            nodeSummary.nodeId = input.nodeId;
            nodeSummary.command = input.command;
            nodeSummary.kernelId = execution.record.kernelId;
            nodeSummary.ok = execution.ok;
            nodeSummary.computeKind = L"static_neighbor_read_v1";
            nodeSummary.resultArtifactId = execution.record.resultArtifactId;
            nodeSummary.resultSha256 = execution.record.resultSha256;
            nodeSummary.logicalSha256 = execution.record.logicalSha256;
            nodeSummary.computeMix64 = execution.record.computeMix64;
            nodeSummary.controlToken = execution.record.controlToken;
            nodeSummary.inputBindingMode = input.inputBindingMode;
            nodeSummary.boundInputSha256 = input.boundInputSha256;
            nodeSummary.boundInputMix64 = input.boundInputMix64;
            nodeSummary.boundInputApplied = true;
            nodeSummary.verified = execution.verified;
            nodeSummary.inputEdgeCount = input.inputEdgeCount;
            nodeSummary.inputEdgesJson = input.inputEdgesJson;
            nodeSummary.resourceUsageJson = WorkerGraphResourceUsageJson(0, 0, 0);
            FinalizeNodeResult(input, nodeSummary, execution);
            return execution;
        }

        WorkerGraphNodeExecutionResult RunControlSelectGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphWideMix64 const& wideMix64)
        {
            auto kernelId = WorkerGraphOptionalString(input.node, L"kernel_id", L"control_select_v1");
            if (kernelId != L"control_select_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_kernel_id_invalid", "control_select requires kernel_id control_select_v1");
            }
            if (input.inputBindingMode != L"control_select_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.input_binding_mode_invalid", "control_select requires input_binding mode control_select_v1");
            }
            if (input.inputEdgeCount < 3)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_edges_required", "control_select requires selector, on_pass and on_fail edges");
            }

            auto passRole = WorkerGraphOptionalString(input.node, L"on_pass_role", L"on_pass");
            auto failRole = WorkerGraphOptionalString(input.node, L"on_fail_role", L"on_fail");
            if (!WorkerGraphIsSafeId(passRole) || !WorkerGraphIsSafeId(failRole) || passRole == failRole)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_role_invalid", "control_select branch roles must be safe and distinct");
            }

            ControlSelectEdge selector;
            ControlSelectEdge passBranch;
            ControlSelectEdge failBranch;
            auto edges = JsonArray::Parse(hstring(input.inputEdgesJson));
            for (uint32_t i = 0; i < edges.Size(); ++i)
            {
                auto value = edges.GetAt(i);
                if (value.ValueType() != JsonValueType::Object)
                {
                    throw WorkerGraphExecutionError("submit_graph.control_select_edge_invalid", "control_select input edge payload is invalid");
                }

                auto parsed = ReadControlSelectEdge(value.GetObject());
                if (parsed.edgeType == L"control" && parsed.role == L"selector")
                {
                    AssignUniqueControlSelectEdge(selector, parsed, "submit_graph.control_select_selector_duplicate");
                }
                else if (parsed.edgeType == L"artifact" && parsed.role == passRole)
                {
                    AssignUniqueControlSelectEdge(passBranch, parsed, "submit_graph.control_select_branch_duplicate");
                }
                else if (parsed.edgeType == L"artifact" && parsed.role == failRole)
                {
                    AssignUniqueControlSelectEdge(failBranch, parsed, "submit_graph.control_select_branch_duplicate");
                }
            }

            if (!selector.present)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_selector_required", "control_select requires one control edge with role selector");
            }
            if (!selector.sourceOk)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_selector_failed", "control_select selector source must be ok");
            }
            if (!passBranch.present || !failBranch.present)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_branch_missing", "control_select requires declared on_pass and on_fail artifact branches");
            }

            auto selectorToken = selector.controlToken;
            if (selectorToken != L"pass" && selectorToken != L"fail")
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_token_invalid", "control_select selector token must be pass or fail");
            }
            auto selectedRole = selectorToken == L"pass" ? passRole : failRole;
            auto selected = selectorToken == L"pass" ? passBranch : failBranch;
            if (!selected.sourceOk)
            {
                throw WorkerGraphExecutionError("submit_graph.control_select_branch_failed", "control_select selected branch source must be ok");
            }

            auto selectSummary =
                std::wstring(L"{\"kernel_id\":\"control_select_v1\"") +
                L",\"compute_kind\":\"canonical_control_select_v1\"" +
                L",\"selector_from_node_id\":" + JsonString(selector.fromNodeId) +
                L",\"selector_token\":" + JsonString(selectorToken) +
                L",\"selected_role\":" + JsonString(selectedRole) +
                L",\"selected_from_node_id\":" + JsonString(selected.fromNodeId) +
                L",\"selected_logical_sha256\":" + JsonString(selected.logicalSha256) +
                L",\"selected_compute_mix64\":" + JsonString(selected.computeMix64) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"deterministic_input_edges\":" + input.deterministicInputEdgesJson +
                L"}";
            auto selectLogicalSha256 = sha256Text(WideToUtf8Local(selectSummary));
            auto selectComputeMix64 = UInt64HexLocal(wideMix64(selectSummary, 0x510e527fade682d1ull));
            auto resultPayload =
                std::wstring(L"{\"schema_version\":\"worker-on-device-graph-control-select-result-0.1\"") +
                L",\"protocol_version\":" + JsonString(input.protocolVersion) +
                L",\"node_id\":" + JsonString(input.nodeId) +
                L",\"kernel_id\":\"control_select_v1\"" +
                L",\"compute_kind\":\"canonical_control_select_v1\"" +
                L",\"input_binding_mode\":\"control_select_v1\"" +
                L",\"input_edge_count\":" + std::to_wstring(input.inputEdgeCount) +
                L",\"selector_from_node_id\":" + JsonString(selector.fromNodeId) +
                L",\"selector_token\":" + JsonString(selectorToken) +
                L",\"selected_role\":" + JsonString(selectedRole) +
                L",\"selected_from_node_id\":" + JsonString(selected.fromNodeId) +
                L",\"selected_result_artifact_id\":" + JsonString(selected.resultArtifactId) +
                L",\"selected_result_sha256\":" + JsonString(selected.resultSha256) +
                L",\"selected_logical_sha256\":" + JsonString(selected.logicalSha256) +
                L",\"selected_compute_mix64\":" + JsonString(selected.computeMix64) +
                L",\"control_select_logical_sha256\":" + JsonString(selectLogicalSha256) +
                L",\"compute_mix64\":" + JsonString(selectComputeMix64) +
                L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
                L",\"verified\":true" +
                L",\"deterministic_input_edges\":" + input.deterministicInputEdgesJson +
                L"}";

            JsonObject publishRequest;
            InsertString(publishRequest, L"artifact_id", input.resultArtifactId);
            InsertString(publishRequest, L"artifact_kind", WorkerGraphOptionalString(input.node, L"artifact_kind", L"on-device-graph-control-select-result"));
            auto manifestFields =
                std::wstring(L",\"job_output_schema\":\"worker-on-device-graph-control-select-result-0.1\"") +
                L",\"kernel_id\":\"control_select_v1\"" +
                L",\"compute_kind\":\"canonical_control_select_v1\"" +
                L",\"selector_token\":" + JsonString(selectorToken) +
                L",\"selected_role\":" + JsonString(selectedRole) +
                L",\"selected_result_artifact_id\":" + JsonString(selected.resultArtifactId) +
                L",\"compute_mix64\":" + JsonString(selectComputeMix64);
            auto published = artifactPublisher(
                publishRequest,
                input.resultArtifactId,
                L"on-device-graph-control-select-result",
                WideToUtf8Local(resultPayload),
                manifestFields);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = true;
            execution.verified = true;
            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = published.artifactId;
            execution.record.resultSha256 = published.sha256;
            execution.record.logicalSha256 = selectLogicalSha256;
            execution.record.computeMix64 = selectComputeMix64;
            execution.record.kernelId = kernelId;
            execution.record.controlToken = selectorToken;
            execution.record.ok = execution.ok;

            WorkerGraphNodeResultSummary nodeSummary;
            nodeSummary.nodeId = input.nodeId;
            nodeSummary.command = input.command;
            nodeSummary.kernelId = execution.record.kernelId;
            nodeSummary.ok = execution.ok;
            nodeSummary.computeKind = L"canonical_control_select_v1";
            nodeSummary.resultArtifactId = execution.record.resultArtifactId;
            nodeSummary.resultSha256 = execution.record.resultSha256;
            nodeSummary.logicalSha256 = execution.record.logicalSha256;
            nodeSummary.computeMix64 = execution.record.computeMix64;
            nodeSummary.controlToken = execution.record.controlToken;
            nodeSummary.inputBindingMode = input.inputBindingMode;
            nodeSummary.boundInputSha256 = input.boundInputSha256;
            nodeSummary.boundInputMix64 = input.boundInputMix64;
            nodeSummary.boundInputApplied = true;
            nodeSummary.verified = execution.verified;
            nodeSummary.inputEdgeCount = input.inputEdgeCount;
            nodeSummary.inputEdgesJson = input.inputEdgesJson;
            nodeSummary.resourceUsageJson = WorkerGraphResourceUsageJson(0, 0, 0);
            FinalizeNodeResult(input, nodeSummary, execution);
            return execution;
        }

        WorkerGraphNodeExecutionResult RunXvmGraphNode(
            WorkerGraphNodeExecutionInput const& input,
            WorkerXvmProgramResolver const& xvmResolver,
            WorkerXvmProgramTextReader const& xvmTextReader,
            WorkerGraphArtifactPublisher const& artifactPublisher,
            WorkerGraphSha256Text const& sha256Text,
            WorkerGraphWideMix64 const& wideMix64,
            std::function<bool()> const& cancelRequested)
        {
            if (input.inputBindingMode != L"typed_xvm_inputs_v1")
            {
                throw WorkerGraphExecutionError("submit_graph.xvm_input_binding_invalid", "xvm_program requires input_binding mode typed_xvm_inputs_v1");
            }

            WorkerXvmExecutionInput xvmInput;
            xvmInput.node = input.node;
            xvmInput.protocolVersion = input.protocolVersion;
            xvmInput.executionId = input.executionId;
            xvmInput.nodeId = input.nodeId;
            xvmInput.resultArtifactId = input.resultArtifactId;
            xvmInput.boundInputSha256 = input.boundInputSha256;
            xvmInput.boundInputMix64 = input.boundInputMix64;
            xvmInput.inputEdgesJson = input.inputEdgesJson;
            xvmInput.snapshotAuthorityProvider = input.snapshotAuthorityProvider;
            xvmInput.cancelRequested = cancelRequested;
            xvmInput.inputControlPass = input.inputControlPass;
            xvmInput.inputEdgeCount = input.inputEdgeCount;
            xvmInput.admittedGpuExecutionPlan = input.admittedGpuExecutionPlan;
            xvmInput.hasAdmittedGpuExecutionPlan = input.hasAdmittedGpuExecutionPlan;
            xvmInput.admittedSpmdExecutionPlan = input.admittedSpmdExecutionPlan;
            xvmInput.hasAdmittedSpmdExecutionPlan = input.hasAdmittedSpmdExecutionPlan;
            xvmInput.admittedCpuExecutionPlan = input.admittedCpuExecutionPlan;
            xvmInput.hasAdmittedCpuExecutionPlan = input.hasAdmittedCpuExecutionPlan;
            xvmInput.admittedCpuCapsulePlan = input.admittedCpuCapsulePlan;
            xvmInput.hasAdmittedCpuCapsulePlan = input.hasAdmittedCpuCapsulePlan;
            xvmInput.admittedBackendConvergencePlan =
                input.admittedBackendConvergencePlan;
            xvmInput.hasAdmittedBackendConvergencePlan =
                input.hasAdmittedBackendConvergencePlan;
            xvmInput.admittedProductionModePlan = input.admittedProductionModePlan;
            xvmInput.hasAdmittedProductionModePlan = input.hasAdmittedProductionModePlan;
            xvmInput.admittedCpuPlanBuildElapsedMs = input.admittedCpuPlanBuildElapsedMs;
            auto xvm = WorkerExecuteXvmProgram(
                xvmInput,
                xvmResolver,
                xvmTextReader,
                artifactPublisher,
                sha256Text,
                wideMix64);

            WorkerGraphNodeExecutionResult execution;
            execution.ok = xvm.ok;
            execution.verified = xvm.verified;
            execution.logicalBytesRead = xvm.programBytes;
            execution.record.nodeId = input.nodeId;
            execution.record.resultArtifactId = xvm.published.artifactId;
            execution.record.resultSha256 = xvm.published.sha256;
            execution.record.logicalSha256 = xvm.logicalSha256;
            execution.record.computeMix64 = xvm.computeMix64;
            execution.record.kernelId = xvm.programId;
            execution.record.controlToken = xvm.controlToken;
            execution.record.fuelConsumed = xvm.fuelConsumed;
            execution.record.memoryBytes = xvm.memoryBytes;
            execution.record.outputBytes = xvm.outputBytes;
            execution.record.ok = xvm.ok;

            WorkerGraphNodeResultSummary summary;
            summary.nodeId = input.nodeId;
            summary.command = input.command;
            summary.kernelId = xvm.programId;
            summary.computeKind = xvm.computeKind;
            summary.manifestArtifactId = WorkerGraphOptionalString(input.node, L"program_artifact_id");
            summary.resultArtifactId = xvm.published.artifactId;
            summary.resultSha256 = xvm.published.sha256;
            summary.logicalSha256 = xvm.logicalSha256;
            summary.logicalBytesRead = xvm.programBytes;
            summary.computeMix64 = xvm.computeMix64;
            summary.controlToken = xvm.controlToken;
            summary.inputBindingMode = input.inputBindingMode;
            summary.boundInputSha256 = input.boundInputSha256;
            summary.boundInputMix64 = input.boundInputMix64;
            summary.boundInputApplied = true;
            summary.verified = xvm.verified;
            summary.ok = xvm.ok;
            summary.inputEdgeCount = input.inputEdgeCount;
            summary.inputEdgesJson = input.inputEdgesJson;
            summary.fuelConsumed = xvm.fuelConsumed;
            summary.memoryBytes = xvm.memoryBytes;
            summary.outputBytes = xvm.outputBytes;
            summary.resourceUsageJson = xvm.resourceUsageJson;
            summary.executionDetailsJson = L"{\"functional_canonical\":" + xvm.functionalCanonicalJson +
                L",\"typed_memory\":" + xvm.typedMemoryJson +
                L",\"structured_control\":" + xvm.structuredControlJson +
                L",\"static_fuel_proof\":" + xvm.staticFuelProofJson +
                L",\"gpu_differential\":" + xvm.gpuDifferentialJson +
                L",\"microtrace_execution\":" + xvm.microtraceExecutionJson +
                L",\"spmd_execution\":" + xvm.spmdExecutionJson +
                L",\"provable_worlds\":" + xvm.provableWorldsJson +
                L",\"backend_convergence\":" + xvm.backendConvergenceJson +
                L",\"production_mode\":" + xvm.productionModeJson +
                L",\"cpu_tiered_execution\":" + xvm.cpuTieredExecutionJson +
                L",\"state_snapshot\":" + xvm.stateSnapshotJson +
                L",\"state_resume\":" + xvm.stateResumeJson + L"}";
            FinalizeNodeResult(input, summary, execution);
            return execution;
        }
    }

    WorkerGraphExecutionError::WorkerGraphExecutionError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue))
    {
    }

    char const* WorkerGraphExecutionError::what() const noexcept
    {
        return message.c_str();
    }

    WorkerGraphNodeExecutionResult WorkerGraphExecuteNode(
        WorkerGraphNodeExecutionInput const& input,
        WorkerGraphComputeNodeRunner const& computeRunner,
        WorkerXvmProgramResolver const& xvmResolver,
        WorkerXvmProgramTextReader const& xvmTextReader,
        WorkerGraphArtifactPublisher const& artifactPublisher,
        WorkerGraphSha256Text const& sha256Text,
        WorkerGraphWideMix64 const& wideMix64,
        std::function<bool()> const& cancelRequested)
    {
        if (input.command == L"run_artifact_manifest_compute_job")
        {
            return RunComputeGraphNode(input, computeRunner);
        }
        if (input.command == L"reduce_graph_verdict")
        {
            return RunReduceGraphNode(input, artifactPublisher, sha256Text, wideMix64);
        }
        if (input.command == L"xvm_program")
        {
            return RunXvmGraphNode(input, xvmResolver, xvmTextReader, artifactPublisher, sha256Text, wideMix64, cancelRequested);
        }
        if (input.command == L"gpu_spmd_precompiled")
        {
            return RunGpuSpmdGraphNode(input, computeRunner, artifactPublisher, sha256Text, wideMix64);
        }
        if (input.command == L"control_select")
        {
            return RunControlSelectGraphNode(input, artifactPublisher, sha256Text, wideMix64);
        }
        if (input.command == L"neighbor_read")
        {
            return RunNeighborReadGraphNode(input, artifactPublisher, sha256Text, wideMix64);
        }
        throw WorkerGraphExecutionError("submit_graph.command_not_allowlisted", "submit_graph allows artifact-manifest compute, reduce_graph_verdict, control_select, neighbor_read, gpu_spmd_precompiled and admitted xvm_program nodes");
    }
}
