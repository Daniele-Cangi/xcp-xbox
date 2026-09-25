#include "pch.h"

#include "WorkerGraphOrchestration.h"

#include "WorkerGraphAdmission.h"
#include "WorkerGraphCheckpointing.h"
#include "WorkerGraphExpansion.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphRuntime.h"
#include "WorkerGraphValidation.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

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

        std::wstring UInt64Hex(uint64_t value)
        {
            std::wostringstream out;
            out << std::hex << std::setfill(L'0') << std::setw(16) << value;
            return out.str();
        }

        double ElapsedMilliseconds(std::chrono::steady_clock::time_point const& started)
        {
            auto elapsed = std::chrono::steady_clock::now() - started;
            return std::chrono::duration<double, std::milli>(elapsed).count();
        }
        static void ThrowIfGraphJobCanceled(WorkerGraphCancelRequested const& cancelRequested)
        {
            if (cancelRequested && cancelRequested())
            {
                throw WorkerGraphValidationError("job.canceled", "graph job was canceled");
            }
        }


    }

    std::wstring WorkerGraphExecuteSubmitGraph(
        JsonObject const& request,
        WorkerGraphOrchestrationServices const& services,
        WorkerGraphCancelRequested const& cancelRequested,
        WorkerGraphCheckpointObserver const& checkpointObserver)
    {
        ThrowIfGraphJobCanceled(cancelRequested);
        auto generatedGraphId = services.generateSessionId();
        if (generatedGraphId.size() > 16)
        {
            generatedGraphId.resize(16);
        }
        generatedGraphId = L"graph-" + generatedGraphId;
        auto const executionPlan = WorkerGraphAdmitExecutionPlan(
            request,
            generatedGraphId,
            services.xvmResolver,
            services.xvmTextReader,
            services.xvmSnapshotAuthorityLoader);
        auto graph = executionPlan.graph;
        auto const& graphId = executionPlan.graphId;
        auto const& schema = executionPlan.graphSchemaVersion;
        auto resourceLedger = executionPlan.resourceLedger;
        auto const& boundedLoop = executionPlan.boundedLoop;
        auto const& staticNeighborRead = executionPlan.staticNeighborRead;
        auto const& graphExpansion = executionPlan.expansion;
        auto const executionPlanJson = WorkerGraphExecutionPlanJson(executionPlan);
        auto const executionPlanSha256 = services.sha256Text(WideToUtf8(executionPlanJson));
        auto started = std::chrono::steady_clock::now();
        std::vector<std::wstring> nodeResults;
        nodeResults.reserve(executionPlan.nodes.size());
        std::vector<std::wstring> canonicalNodeResults;
        canonicalNodeResults.reserve(executionPlan.nodes.size());
        std::vector<std::wstring> executedNodeIds;
        executedNodeIds.reserve(executionPlan.nodes.size());
        std::map<std::wstring, WorkerGraphNodeRecord> completedNodes;
        WorkerGraphCheckpointJournal checkpointJournal(
            request,
            graph,
            graphId,
            services.protocolVersion,
            services.artifactPublisher,
            services.sha256Text,
            checkpointObserver);
        uint64_t passedNodeCount = 0;
        uint64_t failedNodeCount = 0;
        uint64_t totalLogicalBytes = 0;
        uint64_t edgeCount = 0;
        for (uint32_t i = 0; i < executionPlan.nodes.size(); ++i)
        {
            ThrowIfGraphJobCanceled(cancelRequested);
            auto const& admittedNode = executionPlan.nodes[i];
            auto node = admittedNode.node;
            auto const& nodeId = admittedNode.nodeId;

            std::vector<std::wstring> inputEdges;
            std::vector<std::wstring> deterministicInputEdges;
            bool inputEdgesAllOk = true;
            bool inputControlPass = true;
            for (auto const& edgeValidation : admittedNode.inputEdges)
            {
                auto sourceIt = completedNodes.find(edgeValidation.fromNodeId);
                if (sourceIt == completedNodes.end())
                {
                    throw WorkerGraphValidationError(
                        "submit_graph.execution_plan_corrupt",
                        "admitted input edge source is unavailable at execution time");
                }
                if (!sourceIt->second.ok)
                {
                    inputEdgesAllOk = false;
                }
                if (edgeValidation.edgeType == L"control" && sourceIt->second.controlToken != L"pass")
                {
                    inputControlPass = false;
                }

                edgeCount += 1;
                inputEdges.push_back(WorkerGraphInputEdgeJson(
                    sourceIt->second,
                    edgeValidation.role,
                    edgeValidation.edgeType,
                    edgeValidation.valueType));
                deterministicInputEdges.push_back(WorkerGraphDeterministicInputEdgeJson(
                    sourceIt->second,
                    edgeValidation.role,
                    edgeValidation.edgeType,
                    edgeValidation.valueType));
            }
            auto inputEdgesJson = WorkerGraphJsonArray(inputEdges);

            std::sort(deterministicInputEdges.begin(), deterministicInputEdges.end());
            auto deterministicInputEdgesJson = WorkerGraphJsonArray(deterministicInputEdges);

            auto const& inputBindingMode = admittedNode.inputBindingMode;
            std::wstring boundInputSha256;
            std::wstring boundInputMix64;
            if (!inputBindingMode.empty())
            {
                boundInputSha256 = services.sha256Text(WideToUtf8(deterministicInputEdgesJson));
                boundInputMix64 = UInt64Hex(services.wideMix64(deterministicInputEdgesJson, 0x3c6ef372fe94f82bull));
            }

            WorkerGraphNodeExecutionInput executionInput;
            executionInput.node = node;
            executionInput.protocolVersion = services.protocolVersion;
            executionInput.executionId = graphId;
            executionInput.nodeId = nodeId;
            executionInput.command = admittedNode.command;
            executionInput.resultArtifactId = admittedNode.resultArtifactId;
            executionInput.inputBindingMode = inputBindingMode;
            executionInput.inputEdgesJson = inputEdgesJson;
            executionInput.deterministicInputEdgesJson = deterministicInputEdgesJson;
            executionInput.boundInputSha256 = boundInputSha256;
            executionInput.boundInputMix64 = boundInputMix64;
            executionInput.snapshotAuthorityProvider = services.xvmSnapshotAuthorityProvider;
            executionInput.inputEdgesAllOk = inputEdgesAllOk;
            executionInput.inputControlPass = inputControlPass;
            executionInput.inputEdgeCount = static_cast<uint64_t>(inputEdges.size());
            executionInput.admittedGpuExecutionPlan = admittedNode.xvmGpuExecutionPlan;
            executionInput.hasAdmittedGpuExecutionPlan = admittedNode.xvmGpuExecutionPlan.admitted;
            executionInput.admittedSpmdExecutionPlan = admittedNode.xvmSpmdExecutionPlan;
            executionInput.hasAdmittedSpmdExecutionPlan = admittedNode.xvmSpmdExecutionPlan.admitted;
            executionInput.admittedCpuExecutionPlan = admittedNode.xvmCpuExecutionPlan;
            executionInput.hasAdmittedCpuExecutionPlan = admittedNode.xvmCpuExecutionPlan.admitted;
            executionInput.admittedCpuCapsulePlan = admittedNode.xvmCpuCapsulePlan;
            executionInput.hasAdmittedCpuCapsulePlan = admittedNode.xvmCpuCapsulePlan.admitted;
            executionInput.admittedBackendConvergencePlan =
                admittedNode.xvmBackendConvergencePlan;
            executionInput.hasAdmittedBackendConvergencePlan =
                admittedNode.xvmBackendConvergencePlan.admitted;
            executionInput.admittedProductionModePlan =
                admittedNode.xvmProductionModePlan;
            executionInput.hasAdmittedProductionModePlan =
                admittedNode.xvmProductionModePlan.admitted;
            executionInput.admittedCpuPlanBuildElapsedMs = admittedNode.xvmCpuPlanBuildElapsedMs;

            auto execution = WorkerGraphExecuteNode(
                executionInput,
                services.computeRunner,
                services.xvmResolver,
                services.xvmTextReader,
                services.artifactPublisher,
                services.sha256Text,
                services.wideMix64,
                cancelRequested);

            WorkerGraphRecordResourceUsage(
                resourceLedger,
                execution.record.fuelConsumed,
                execution.record.memoryBytes,
                execution.record.outputBytes);

            if (execution.ok)
            {
                ++passedNodeCount;
            }
            else
            {
                ++failedNodeCount;
            }
            totalLogicalBytes += execution.logicalBytesRead;
            executedNodeIds.push_back(nodeId);
            completedNodes[nodeId] = execution.record;
            nodeResults.push_back(execution.nodeResultJson);
            canonicalNodeResults.push_back(execution.canonicalNodeResultJson);

            auto completedNodesJson = WorkerGraphJsonArray(nodeResults);
            auto canonicalCompletedNodesJson = WorkerGraphJsonArray(canonicalNodeResults);
            WorkerGraphCheckpointRecordInput checkpointInput;
            checkpointInput.completedNodeCount = static_cast<uint64_t>(nodeResults.size());
            checkpointInput.lastNodeId = nodeId;
            checkpointInput.edgeCount = edgeCount;
            checkpointInput.passedNodeCount = passedNodeCount;
            checkpointInput.failedNodeCount = failedNodeCount;
            checkpointInput.totalLogicalBytes = totalLogicalBytes;
            checkpointInput.resourceLedgerJson = WorkerGraphResourceLedgerJson(resourceLedger);
            checkpointInput.completedNodesJson = completedNodesJson;
            checkpointInput.canonicalCompletedNodesJson = canonicalCompletedNodesJson;
            checkpointJournal.Publish(checkpointInput);
            ThrowIfGraphJobCanceled(cancelRequested);
        }

        auto nodesJson = WorkerGraphJsonArray(nodeResults);
        auto canonicalNodesJson = WorkerGraphJsonArray(canonicalNodeResults);
        auto graphResultSha256 = services.sha256Text(WideToUtf8(nodesJson));
        auto graphResultCanonicalSha256 = services.sha256Text(WideToUtf8(canonicalNodesJson));
        auto const checkpoint = checkpointJournal.Finalize(
            static_cast<uint64_t>(executionPlan.nodes.size()),
            graphResultCanonicalSha256,
            graphResultSha256);
        ThrowIfGraphJobCanceled(cancelRequested);
        auto elapsedMs = ElapsedMilliseconds(started);
        auto verdict = failedNodeCount == 0 ? L"PASS" : L"FAIL";
        auto graphSpecJson = std::wstring(graph.Stringify().c_str());
        auto graphSpecSha256 = services.sha256Text(WideToUtf8(graphSpecJson));
        auto nodeLineageSha256 = services.sha256Text(WideToUtf8(nodesJson));
        auto canonicalNodeLineageSha256 = graphResultCanonicalSha256;
        WorkerGraphPublishInput publishInput;
        publishInput.request = request;
        publishInput.graph = graph;
        publishInput.protocolVersion = services.protocolVersion;
        publishInput.graphSchemaVersion = schema;
        publishInput.graphId = graphId;
        publishInput.executionPlanSchemaVersion = WorkerGraphExecutionPlanSchemaVersion;
        publishInput.executionPlanJson = executionPlanJson;
        publishInput.executionPlanSha256 = executionPlanSha256;
        publishInput.fullGraphPreAdmission = true;
        publishInput.preAdmittedNodeCount = static_cast<uint64_t>(executionPlan.nodes.size());
        publishInput.preAdmittedEdgeCount = executionPlan.edgeCount;
        publishInput.nodesJson = nodesJson;
        publishInput.canonicalNodesJson = canonicalNodesJson;
        publishInput.graphResultSha256 = graphResultSha256;
        publishInput.graphResultCanonicalSha256 = graphResultCanonicalSha256;
        publishInput.graphSpecSha256 = graphSpecSha256;
        publishInput.nodeLineageSha256 = nodeLineageSha256;
        publishInput.canonicalNodeLineageSha256 = canonicalNodeLineageSha256;
        publishInput.verdict = verdict;
        publishInput.nodeCount = static_cast<uint32_t>(executionPlan.nodes.size());
        publishInput.edgeCount = edgeCount;
        publishInput.passedNodeCount = passedNodeCount;
        publishInput.failedNodeCount = failedNodeCount;
        publishInput.totalLogicalBytes = totalLogicalBytes;
        publishInput.resourceLedgerJson = WorkerGraphResourceLedgerJson(resourceLedger);
        publishInput.xvmNodeCount = resourceLedger.xvmNodeCount;
        publishInput.boundedLoopProfileAdmitted = boundedLoop.present;
        publishInput.boundedLoopProfileJson = boundedLoop.profileJson;
        publishInput.boundedLoopIterationCount = boundedLoop.iterationCount;
        publishInput.boundedLoopMaxIterations = boundedLoop.maxIterations;
        publishInput.boundedLoopFuelBudget = boundedLoop.fuelBudget;
        publishInput.boundedLoopFuelConsumed = boundedLoop.fuelBudget;
        publishInput.staticNeighborReadProfileAdmitted = staticNeighborRead.present;
        publishInput.staticNeighborReadProfileJson = staticNeighborRead.profileJson;
        publishInput.staticNeighborReadNeighborCount = staticNeighborRead.neighborCount;
        publishInput.graphExpansionProfileAdmitted = graphExpansion.present;
        publishInput.graphExpansionProfileJson = graphExpansion.profileJson;
        publishInput.graphExpansionPreNodeCount = graphExpansion.preNodeCount;
        publishInput.graphExpansionExpandedNodeCount = graphExpansion.expandedNodeCount;
        publishInput.graphExpansionExpandedEdgeCount = graphExpansion.expandedEdgeCount;
        publishInput.checkpointLogJson = checkpoint.logJson;
        publishInput.checkpointArtifactsJson = checkpoint.artifactsJson;
        publishInput.checkpointChainSha256 = checkpoint.chainSha256;
        publishInput.checkpointReplayJson = checkpoint.replayJson;
        publishInput.checkpointCount = checkpoint.count;
        publishInput.checkpointReplayVerified = checkpoint.replayVerified;

        auto graphPublished = WorkerGraphPublishArtifacts(
            publishInput,
            services.artifactPublisher,
            services.sha256Text);

        ThrowIfGraphJobCanceled(cancelRequested);

        return WorkerGraphSubmitGraphResponse(publishInput, graphPublished, elapsedMs);
    }

}
