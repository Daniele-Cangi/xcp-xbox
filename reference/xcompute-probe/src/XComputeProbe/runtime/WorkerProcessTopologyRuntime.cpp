#include "pch.h"
#include "WorkerProcessTopologyRuntime.h"

#include "WorkerProcessTopologyIpcProjection.h"
#include "WorkerProcessTopologyMemoryProbe.h"
#include "WorkerProcessTopologyProtocol.h"
#include "WorkerProcessTopologyRefusalEvidence.h"
#include "WorkerProcessTopologyStorageProbe.h"
#include "WorkerProcessTopologyWorkloadWitness.h"
#include "WorkerProcessTopologyTransport.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::System;

namespace XComputeProbe
{
    using namespace ProcessTopology;

    namespace
    {
        std::wstring VersionString(PackageVersion const& version)
        {
            std::wostringstream out;
            out << version.Major << L'.'
                << version.Minor << L'.'
                << version.Build << L'.'
                << version.Revision;
            return out.str();
        }

    }


    std::wstring WorkerProcessTopologyRuntime::Probe(
        JsonObject const& request,
        std::wstring const& protocolVersion)
    {
        std::wstring correlationId = L"topology-probe";
        MonotonicDeadline deadline;
        try
        {
            if (!request || !IsSafeToken(protocolVersion, 32))
            {
                Fail(
                    L"topology.request_schema_invalid",
                    L"the topology request or protocol is invalid");
            }
            auto requestSchema = RequiredString(
                request,
                L"schema_version",
                L"topology.request_schema_invalid");
            auto phaseInstrumented =
                WorkerProcessTopologyLifecycleRuntime::
                    IsPhaseRequestSchema(requestSchema);
            if (requestSchema != RequestSchema &&
                !phaseInstrumented)
            {
                Fail(
                    L"topology.request_schema_invalid",
                    L"the topology runtime request schema_version is unsupported",
                    requestSchema);
            }
            if (request.HasKey(L"correlation_id"))
            {
                correlationId = RequiredString(
                    request,
                    L"correlation_id",
                    L"topology.request_schema_invalid");
            }
            if (!IsSafeToken(correlationId, 64))
            {
                Fail(
                    L"topology.request_schema_invalid",
                    L"correlation_id must be a bounded non-secret token");
            }

            BrokerPhaseBinding phaseBinding;
            std::wstring lifecycleStage = L"none";
            if (phaseInstrumented)
            {
                auto command = RequiredString(
                    request,
                    L"command",
                    L"topology.request_schema_invalid");
                if (command != L"probe_process_topology")
                {
                    Fail(
                        L"topology.request_schema_invalid",
                        L"the T2 phase request command is invalid",
                        command);
                }
                phaseBinding =
                    ParseBrokerPhaseBindingRequest(request);
                lifecycleStage = lifecycleRuntime_.ValidateLifecycleStage(
                    request, requestSchema, phaseBinding);
            }

            auto workerTouchedBytes = RequiredUInt64(
                request,
                L"worker_touched_bytes",
                L"topology.request_schema_invalid");
            auto brokerTouchedBytes = RequiredUInt64(
                request,
                L"broker_touched_bytes",
                L"topology.request_schema_invalid");
            auto heartbeatCount = RequiredUInt64(
                request,
                L"heartbeat_count",
                L"topology.request_schema_invalid");
            auto ipcPayloadBytes = RequiredUInt64(
                request,
                L"ipc_payload_bytes",
                L"topology.request_schema_invalid");
            auto holdIntervalMilliseconds = RequiredUInt64(
                request,
                L"hold_interval_ms",
                L"topology.request_schema_invalid");
            auto storageProbeBytes = phaseInstrumented
                ? RequiredUInt64(
                    request,
                    L"storage_probe_bytes",
                    L"topology.request_schema_invalid")
                : 0;

            constexpr uint64_t minimumTouchedBytes =
                1024ull * 1024ull;
            if (workerTouchedBytes < minimumTouchedBytes ||
                brokerTouchedBytes < minimumTouchedBytes ||
                workerTouchedBytes > MaximumTouchedBytes ||
                brokerTouchedBytes > MaximumTouchedBytes ||
                heartbeatCount == 0 ||
                heartbeatCount > MaximumHeartbeatCount ||
                ipcPayloadBytes == 0 ||
                ipcPayloadBytes > MaximumPayloadBytes ||
                holdIntervalMilliseconds < 10 ||
                holdIntervalMilliseconds >
                    MaximumHoldIntervalMilliseconds ||
                heartbeatCount >
                    MaximumHeartbeatWindowMilliseconds /
                        holdIntervalMilliseconds)
            {
                Fail(
                    L"topology.memory_request_invalid",
                    L"the memory, heartbeat, hold, or payload request is outside Gate T2");
            }

            auto storagePhase = phaseInstrumented &&
                phaseBinding.phaseId == L"storage_visibility";
            if ((storagePhase &&
                    (storageProbeBytes == 0 ||
                     storageProbeBytes > MaximumStorageProbeBytes)) ||
                (!storagePhase && storageProbeBytes != 0))
            {
                Fail(
                    L"topology.storage_probe_failed",
                    L"storage_probe_bytes must be nonzero only for the bounded storage_visibility phase");
            }

            auto aggregateTouchedPerRound =
                workerTouchedBytes + brokerTouchedBytes;
            auto aggregateTouchedRounds =
                heartbeatCount + 1;
            if (aggregateTouchedPerRound >
                    MaximumAggregateTouchedRoundBytes /
                        aggregateTouchedRounds ||
                ipcPayloadBytes >
                    MaximumAggregatePayloadRoundBytes /
                        heartbeatCount)
            {
                Fail(
                    L"topology.memory_request_invalid",
                    L"the aggregate touched-memory or payload round budget exceeds Gate T2");
            }

            SYSTEM_INFO systemInfo{};
            GetSystemInfo(&systemInfo);
            auto pageBytes =
                static_cast<uint64_t>(systemInfo.dwPageSize);
            if (pageBytes == 0)
            {
                Fail(
                    L"topology.memory_allocation_failed",
                    L"the system page size was unavailable");
            }
            auto brokerCommittedBytes =
                ((brokerTouchedBytes + pageBytes - 1) /
                    pageBytes) * pageBytes;

            auto package = Package::Current();
            auto packageId = package.Id();
            auto packageFamily =
                std::wstring(packageId.FamilyName().c_str());
            auto packageFullName =
                std::wstring(packageId.FullName().c_str());
            auto foregroundProcess =
                CaptureProcessIdentity();
            auto memoryBefore = CaptureMemorySample();

            std::mutex limitMutex;
            std::vector<LimitChange> limitChanges;
            auto limitRevoker =
                MemoryManager::AppMemoryUsageLimitChanging(
                    auto_revoke,
                    [&](IInspectable const&,
                        AppMemoryUsageLimitChangingEventArgs const& args)
                    {
                        std::lock_guard<std::mutex> lock(limitMutex);
                        limitChanges.push_back(LimitChange{
                            UnixMilliseconds(),
                            args.OldLimit(),
                            args.NewLimit() });
                    });

            WorkerProcessTopologyTransport transport;
            auto openReply = transport.Open(
                AppServiceName,
                packageFamily,
                deadline.Remaining());
            auto openMilliseconds =
                openReply.roundTripMilliseconds;
            if (!openReply.succeeded)
            {
                Fail(
                    L"topology.app_service_open_failed",
                    L"the same-package topology App Service did not open",
                    std::to_wstring(openReply.statusCode));
            }

            std::vector<double> ipcLatencies;
            uint64_t ipcResponseBytes = 0;
            uint64_t ipcCommandsCompleted = 0;

            auto describeReply = SendBrokerCommand(
                transport,
                NewBrokerCommand(L"describe"),
                deadline.Remaining());
            ipcLatencies.push_back(
                describeReply.roundTripMilliseconds);
            ipcResponseBytes +=
                describeReply.responseUtf8Bytes;
            ++ipcCommandsCompleted;

            auto describeMetrics = RequiredObject(
                describeReply.result,
                L"metrics",
                L"topology.app_service_payload_invalid");
            auto brokerPackage = RequiredObject(
                describeMetrics,
                L"package_identity",
                L"topology.app_service_payload_invalid");
            auto brokerFamily = RequiredString(
                brokerPackage,
                L"family_name",
                L"topology.app_service_payload_invalid");
            auto brokerFullName = RequiredString(
                brokerPackage,
                L"full_name",
                L"topology.app_service_payload_invalid");
            auto brokerProcessId = RequiredUInt64(
                describeMetrics,
                L"process_id",
                L"topology.app_service_payload_invalid");
            if (brokerFamily != packageFamily ||
                brokerFullName != packageFullName ||
                brokerProcessId == 0 ||
                brokerProcessId ==
                    foregroundProcess.processId)
            {
                Fail(
                    L"topology.app_service_payload_invalid",
                    L"the broker is not a distinct process in the foreground package");
            }

            bool brokerAllocationActive = false;
            try
            {
                deadline.RequireMeasuredWindow(
                    heartbeatCount,
                    holdIntervalMilliseconds);
                MeasuredHold foregroundHold(
                    workerTouchedBytes,
                    heartbeatCount,
                    holdIntervalMilliseconds);
                foregroundHold.Start();

                auto allocateReply = AllocateBrokerWithRefusalEvidence(
                    transport,
                    deadline,
                    lifecycleRuntime_,
                    foregroundHold,
                    memoryBefore,
                    limitMutex,
                    limitChanges,
                    packageFullName,
                    foregroundProcess,
                    brokerFullName,
                    brokerProcessId,
                    describeReply.activationId,
                    workerTouchedBytes,
                    brokerTouchedBytes,
                    brokerCommittedBytes,
                    heartbeatCount,
                    holdIntervalMilliseconds);
                brokerAllocationActive = true;
                ipcLatencies.push_back(
                    allocateReply.roundTripMilliseconds);
                ipcResponseBytes +=
                    allocateReply.responseUtf8Bytes;
                ++ipcCommandsCompleted;

                JsonObject phaseBeginResult{ nullptr };
                JsonObject phaseCompleteResult{ nullptr };
                JsonObject storageVisibility{ nullptr };
                JsonObject phaseCompleteReplayResult{ nullptr };
                BrokerPhaseStatus phaseBeginStatus;
                BrokerPhaseStatus phaseCompletedStatus;
                if (phaseInstrumented)
                {
                    auto beginReply = SendBrokerCommand(
                        transport,
                        NewBrokerPhaseBeginCommand(phaseBinding),
                        deadline.Remaining());
                    ipcLatencies.push_back(
                        beginReply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        beginReply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;
                    if (beginReply.activationId !=
                        describeReply.activationId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker activation changed during phase begin");
                    }
                    auto beginTransition =
                        ParseBrokerPhaseTransition(
                            beginReply.result,
                            L"BEGIN");
                    if (beginTransition.idempotentSuccess ||
                        !BrokerPhaseMatches(
                            beginTransition.status,
                            phaseBinding,
                            L"ACTIVE"))
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker did not begin the requested T2 phase exactly once");
                    }
                    phaseBeginStatus = beginTransition.status;
                    phaseBeginResult = beginReply.result;
                }

                auto queryPhaseStatus = [&]()
                {
                    auto reply = SendBrokerCommand(
                        transport,
                        NewBrokerCommand(L"phase_status"),
                        deadline.Remaining());
                    ipcLatencies.push_back(
                        reply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        reply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;
                    if (reply.activationId !=
                        describeReply.activationId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker activation changed while reading phase status");
                    }
                    return ParseBrokerPhaseStatus(
                        reply.result);
                };
                auto phaseStatusBefore =
                    queryPhaseStatus();
                if (phaseInstrumented)
                {
                    if (!BrokerPhaseMatches(
                            phaseStatusBefore,
                            phaseBinding,
                            L"ACTIVE") ||
                        phaseStatusBefore.phaseRunId !=
                            phaseBeginStatus.phaseRunId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker phase status lost the active T2 binding after begin");
                    }
                }
                else if (phaseStatusBefore.phaseState !=
                    L"UNASSIGNED")
                {
                    Fail(
                        L"topology.app_service_payload_invalid",
                        L"the preflight probe unexpectedly mutated broker phase state");
                }

                JsonObject lastBrokerAllocation{ nullptr };
                JsonObject lastBrokerMetrics{ nullptr };
                std::wstring lastBrokerDigest;
                uint64_t brokerPreviousStarted = 0;
                uint64_t brokerStarted = 0;
                uint64_t brokerEnded = 0;
                uint64_t brokerMaximumGap = 0;
                uint64_t brokerHeartbeatsObserved = 0;
                uint64_t payloadRoundsCompleted = 0;
                auto expectedPayloadDigest =
                    PayloadSha256(ipcPayloadBytes);
                std::wstring payload(
                    static_cast<size_t>(ipcPayloadBytes),
                    L'x');

                WorkerProcessTopologyWorkloadSampler workloadSampler;

                for (uint64_t index = 1;
                     index <= heartbeatCount;
                     ++index)
                {
                    workloadSampler.Sample();
                    auto heartbeatStarted =
                        UnixMilliseconds();
                    if (brokerStarted == 0)
                    {
                        brokerStarted = heartbeatStarted;
                    }
                    auto gap = brokerPreviousStarted == 0
                        ? 0
                        : heartbeatStarted -
                            brokerPreviousStarted;
                    brokerMaximumGap = (std::max)(
                        brokerMaximumGap,
                        gap);
                    brokerPreviousStarted =
                        heartbeatStarted;

                    auto heartbeatRequest =
                        NewBrokerCommand(L"heartbeat");
                    PutBool(heartbeatRequest, L"mutate", true);
                    auto heartbeatReply =
                        SendBrokerCommand(
                            transport,
                            heartbeatRequest,
                            deadline.Remaining());
                    ipcLatencies.push_back(
                        heartbeatReply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        heartbeatReply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;
                    if (heartbeatReply.activationId !=
                        describeReply.activationId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker activation changed during retention");
                    }

                    auto brokerCount = RequiredUInt64(
                        heartbeatReply.result,
                        L"heartbeat_count",
                        L"topology.app_service_payload_invalid");
                    auto mutated = RequiredBool(
                        heartbeatReply.result,
                        L"mutated",
                        L"topology.app_service_payload_invalid");
                    auto retentionVerified = RequiredBool(
                        heartbeatReply.result,
                        L"retention_verified",
                        L"topology.app_service_payload_invalid");
                    lastBrokerAllocation = RequiredObject(
                        heartbeatReply.result,
                        L"allocation",
                        L"topology.app_service_payload_invalid");
                    lastBrokerMetrics = RequiredObject(
                        heartbeatReply.result,
                        L"metrics",
                        L"topology.app_service_payload_invalid");
                    auto active = RequiredBool(
                        lastBrokerAllocation,
                        L"active",
                        L"topology.app_service_payload_invalid");
                    auto committed = RequiredUInt64(
                        lastBrokerAllocation,
                        L"committed_bytes",
                        L"topology.app_service_payload_invalid");
                    auto requested = RequiredUInt64(
                        lastBrokerAllocation,
                        L"requested_bytes",
                        L"topology.app_service_payload_invalid");
                    auto pageCount = RequiredUInt64(
                        lastBrokerAllocation,
                        L"page_count",
                        L"topology.app_service_payload_invalid");
                    lastBrokerDigest = RequiredString(
                        lastBrokerAllocation,
                        L"digest_sha256",
                        L"topology.app_service_payload_invalid");
                    if (!retentionVerified || !mutated || !active ||
                        committed != brokerCommittedBytes ||
                        requested != brokerCommittedBytes ||
                        pageCount == 0 ||
                        brokerCount == 0 ||
                        !IsLowerHex(lastBrokerDigest, 64))
                    {
                        Fail(
                            L"topology.memory_verification_failed",
                            L"a broker heartbeat failed retained-memory verification");
                    }
                    ++brokerHeartbeatsObserved;

                    auto payloadRequest =
                        NewBrokerCommand(L"payload");
                    PutString(payloadRequest, L"payload", payload);
                    PutBool(payloadRequest, L"echo", false);
                    auto payloadReply =
                        SendBrokerCommand(
                            transport,
                            payloadRequest,
                            deadline.Remaining());
                    ipcLatencies.push_back(
                        payloadReply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        payloadReply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;

                    auto payloadCount = RequiredUInt64(
                        payloadReply.result,
                        L"payload_utf8_bytes",
                        L"topology.app_service_payload_invalid");
                    auto payloadDigest = RequiredString(
                        payloadReply.result,
                        L"digest_sha256",
                        L"topology.app_service_payload_invalid");
                    if (payloadCount != ipcPayloadBytes ||
                        payloadDigest != expectedPayloadDigest)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker payload count or digest did not round-trip");
                    }
                    ++payloadRoundsCompleted;
                    deadline.Sleep(holdIntervalMilliseconds);
                    workloadSampler.Sample();
                }
                workloadSampler.Sample();
                brokerEnded = UnixMilliseconds();
                deadline.Remaining();
                foregroundHold.Join();
                deadline.Remaining();
                if (storagePhase)
                {
                    auto storageProbe =
                        WorkerProcessTopologyStorageProbe::Run(
                            transport,
                            phaseBeginStatus.phaseRunId,
                            storageProbeBytes,
                            deadline);
                    storageVisibility = storageProbe.measurement;
                    for (auto const& reply : storageProbe.brokerReplies)
                    {
                        ipcLatencies.push_back(reply.roundTripMilliseconds);
                        ipcResponseBytes += reply.responseUtf8Bytes;
                        ++ipcCommandsCompleted;
                    }
                }
                auto phaseStatusAfter =
                    queryPhaseStatus();
                if (phaseStatusAfter.sequence <=
                    phaseStatusBefore.sequence)
                {
                    deadline.Sleep(
                        phaseStatusBefore.intervalMilliseconds * 2);
                    phaseStatusAfter =
                        queryPhaseStatus();
                }
                if (phaseStatusAfter.sequence <=
                        phaseStatusBefore.sequence ||
                    phaseStatusAfter.startedMonotonicMilliseconds !=
                        phaseStatusBefore.startedMonotonicMilliseconds ||
                    phaseStatusAfter.lastTickMonotonicMilliseconds <=
                        phaseStatusBefore.lastTickMonotonicMilliseconds)
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"the broker autonomous heartbeat did not advance while both allocations were retained");
                }
                if (phaseInstrumented)
                {
                    if (!BrokerPhaseMatches(
                            phaseStatusAfter,
                            phaseBinding,
                            L"ACTIVE") ||
                        phaseStatusAfter.phaseRunId !=
                            phaseBeginStatus.phaseRunId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the active T2 phase binding changed during the measured window");
                    }
                    auto completeRequest =
                        NewBrokerPhaseCompleteCommand(
                            phaseBinding,
                            phaseBeginStatus.phaseRunId);
                    auto completeReply = SendBrokerCommand(
                        transport,
                        completeRequest,
                        deadline.Remaining());
                    ipcLatencies.push_back(
                        completeReply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        completeReply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;
                    if (completeReply.activationId !=
                        describeReply.activationId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker activation changed during phase completion");
                    }
                    auto completeTransition =
                        ParseBrokerPhaseTransition(
                            completeReply.result,
                            L"COMPLETE");
                    if (completeTransition.idempotentSuccess ||
                        !BrokerPhaseMatches(
                            completeTransition.status,
                            phaseBinding,
                            L"COMPLETED") ||
                        completeTransition.status.phaseRunId !=
                            phaseBeginStatus.phaseRunId ||
                        completeTransition.status.
                            heartbeatSamplesObserved == 0)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker did not complete the T2 phase with an autonomous heartbeat witness");
                    }
                    phaseCompletedStatus =
                        completeTransition.status;
                    phaseCompleteResult = completeReply.result;

                    auto replayReply = SendBrokerCommand(
                        transport,
                        completeRequest,
                        deadline.Remaining());
                    ipcLatencies.push_back(
                        replayReply.roundTripMilliseconds);
                    ipcResponseBytes +=
                        replayReply.responseUtf8Bytes;
                    ++ipcCommandsCompleted;
                    if (replayReply.activationId !=
                        describeReply.activationId)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the broker activation changed during idempotent phase completion replay");
                    }
                    auto replayTransition =
                        ParseBrokerPhaseTransition(
                            replayReply.result,
                            L"COMPLETE");
                    if (!replayTransition.idempotentSuccess ||
                        !BrokerPhaseMatches(
                            replayTransition.status,
                            phaseBinding,
                            L"COMPLETED") ||
                        replayTransition.status.phaseRunId !=
                            phaseCompletedStatus.phaseRunId ||
                        replayTransition.status.transitionSequence !=
                            phaseCompletedStatus.transitionSequence ||
                        replayTransition.status.
                            heartbeatSequenceAtPhaseComplete !=
                            phaseCompletedStatus.
                                heartbeatSequenceAtPhaseComplete)
                    {
                        Fail(
                            L"topology.app_service_payload_invalid",
                            L"the repeated T2 phase completion was not an exact idempotent replay");
                    }
                    phaseCompleteReplayResult = replayReply.result;
                }
                else if (phaseStatusAfter.phaseState !=
                    L"UNASSIGNED")
                {
                    Fail(
                        L"topology.app_service_payload_invalid",
                        L"the preflight probe mutated broker phase state during observation");
                }

                auto lifecycleSnapshot = lifecycleRuntime_.CaptureBrokerSnapshot(
                    transport, deadline, describeReply.activationId);
                ipcLatencies.push_back(lifecycleSnapshot.roundTripMilliseconds);
                ipcResponseBytes += lifecycleSnapshot.responseUtf8Bytes;
                ++ipcCommandsCompleted;

                auto memoryHeld = CaptureMemorySample();
                if (transport.ServiceClosedObserved())
                {
                    Fail(
                        L"topology.app_service_response_failed",
                        L"the broker App Service closed during the measured window");
                }
                if (foregroundHold.HeartbeatCount() != heartbeatCount ||
                    brokerHeartbeatsObserved != heartbeatCount ||
                    payloadRoundsCompleted != heartbeatCount ||
                    foregroundHold.MismatchCount() != 0 ||
                    !IsLowerHex(foregroundHold.DigestSha256(), 64))
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"the exact worker, broker, or payload round contract failed");
                }

                auto overlapStarted = (std::max)(
                    foregroundHold.StartedUnixMilliseconds(),
                    brokerStarted);
                auto overlapEnded = (std::min)(
                    foregroundHold.EndedUnixMilliseconds(),
                    brokerEnded);
                auto overlapMilliseconds =
                    overlapEnded > overlapStarted
                        ? overlapEnded - overlapStarted
                        : 0;
                if (overlapMilliseconds == 0)
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"worker and broker verified-memory rounds did not overlap");
                }

                auto brokerAppLimit = RequiredUInt64(
                    lastBrokerMetrics,
                    L"app_memory_usage_limit_bytes",
                    L"topology.app_service_payload_invalid");
                auto brokerAppUsage = RequiredUInt64(
                    lastBrokerMetrics,
                    L"app_memory_usage_bytes",
                    L"topology.app_service_payload_invalid");
                auto brokerPrivatePages = NestedUInt64(
                    lastBrokerMetrics,
                    L"process_diagnostics_memory",
                    L"private_page_bytes");
                auto brokerWorkingSet = NestedUInt64(
                    lastBrokerMetrics,
                    L"process_diagnostics_memory",
                    L"working_set_bytes");
                auto brokerLimitChanges = OptionalUInt64(
                    lastBrokerMetrics,
                    L"memory_limit_event_count");
                auto brokerCpuBefore = OptionalObject(
                    describeMetrics,
                    L"process_diagnostics_cpu");
                auto brokerCpuAfter = OptionalObject(
                    lastBrokerMetrics,
                    L"process_diagnostics_cpu");
                auto brokerCpuReportAvailable =
                    static_cast<bool>(brokerCpuBefore) &&
                    static_cast<bool>(brokerCpuAfter);
                auto brokerCpuUserBefore = NestedUInt64(
                    describeMetrics,
                    L"process_diagnostics_cpu",
                    L"user_time_100ns");
                auto brokerCpuKernelBefore = NestedUInt64(
                    describeMetrics,
                    L"process_diagnostics_cpu",
                    L"kernel_time_100ns");
                auto brokerCpuUserAfter = NestedUInt64(
                    lastBrokerMetrics,
                    L"process_diagnostics_cpu",
                    L"user_time_100ns");
                auto brokerCpuKernelAfter = NestedUInt64(
                    lastBrokerMetrics,
                    L"process_diagnostics_cpu",
                    L"kernel_time_100ns");
                auto brokerCpuDelta100ns = brokerCpuReportAvailable &&
                    brokerCpuUserAfter >= brokerCpuUserBefore &&
                    brokerCpuKernelAfter >= brokerCpuKernelBefore
                    ? (brokerCpuUserAfter - brokerCpuUserBefore) +
                        (brokerCpuKernelAfter - brokerCpuKernelBefore)
                    : 0;
                auto brokerCpuObserved = brokerCpuReportAvailable &&
                    brokerCpuDelta100ns > 0;

                auto firstReleaseReply = SendBrokerCommand(
                    transport,
                    NewBrokerCommand(L"release"),
                    deadline.Remaining());
                ipcLatencies.push_back(
                    firstReleaseReply.roundTripMilliseconds);
                ipcResponseBytes +=
                    firstReleaseReply.responseUtf8Bytes;
                ++ipcCommandsCompleted;
                auto firstReleased = RequiredBool(
                    firstReleaseReply.result,
                    L"released",
                    L"topology.app_service_payload_invalid");
                auto firstIdempotent = RequiredBool(
                    firstReleaseReply.result,
                    L"idempotent_success",
                    L"topology.app_service_payload_invalid");
                if (!firstReleased || !firstIdempotent)
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"the broker allocation was not explicitly released");
                }
                brokerAllocationActive = false;

                auto secondReleaseReply = SendBrokerCommand(
                    transport,
                    NewBrokerCommand(L"release"),
                    deadline.Remaining());
                ipcLatencies.push_back(
                    secondReleaseReply.roundTripMilliseconds);
                ipcResponseBytes +=
                    secondReleaseReply.responseUtf8Bytes;
                ++ipcCommandsCompleted;
                if (!RequiredBool(
                    secondReleaseReply.result,
                    L"idempotent_success",
                    L"topology.app_service_payload_invalid"))
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"the repeated broker release was not idempotent");
                }

                auto foregroundCommittedBytes =
                    foregroundHold.CommittedBytes();
                auto foregroundDigest =
                    foregroundHold.DigestSha256();
                auto foregroundStarted =
                    foregroundHold.StartedUnixMilliseconds();
                auto foregroundEnded =
                    foregroundHold.EndedUnixMilliseconds();
                if (!foregroundHold.Release())
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"the foreground allocation was not explicitly released");
                }
                auto memoryAfter = CaptureMemorySample();
                auto clientCloseCompleted = transport.Close();

                std::vector<LimitChange> limitSnapshot;
                {
                    std::lock_guard<std::mutex> lock(limitMutex);
                    limitSnapshot = limitChanges;
                }

                auto foregroundCpuReportAvailable =
                    memoryBefore.cpuReportAvailable &&
                    memoryHeld.cpuReportAvailable;
                auto foregroundCpuDelta100ns =
                    foregroundCpuReportAvailable &&
                    memoryHeld.cpuUserTime100ns >= memoryBefore.cpuUserTime100ns &&
                    memoryHeld.cpuKernelTime100ns >= memoryBefore.cpuKernelTime100ns
                    ? (memoryHeld.cpuUserTime100ns - memoryBefore.cpuUserTime100ns) +
                        (memoryHeld.cpuKernelTime100ns - memoryBefore.cpuKernelTime100ns)
                    : 0;
                auto foregroundCpuObserved =
                    foregroundCpuReportAvailable &&
                    foregroundCpuDelta100ns > 0;

                auto foregroundHeartbeat = HeartbeatProjection(
                    holdIntervalMilliseconds,
                    heartbeatCount,
                    foregroundHold.HeartbeatCount(),
                    foregroundHold.MaximumGapMilliseconds(),
                    true);
                auto brokerHeartbeat =
                    NewBrokerAutonomousHeartbeatProjection(
                        phaseStatusBefore,
                        phaseStatusAfter);
                auto foregroundMemory = MemoryProjection(
                    memoryHeld.appLimitBytes,
                    memoryHeld.appUsageBytes,
                    memoryHeld.privatePageCountBytes,
                    memoryHeld.workingSetBytes,
                    foregroundCommittedBytes,
                    workerTouchedBytes,
                    foregroundCommittedBytes,
                    foregroundDigest,
                    heartbeatCount,
                    static_cast<double>(
                        foregroundEnded - foregroundStarted) / 1000.0,
                    limitSnapshot.size());
                auto brokerMemory = MemoryProjection(
                    brokerAppLimit,
                    brokerAppUsage,
                    brokerPrivatePages,
                    brokerWorkingSet,
                    brokerCommittedBytes,
                    brokerTouchedBytes,
                    brokerCommittedBytes,
                    lastBrokerDigest,
                    heartbeatCount,
                    static_cast<double>(
                        brokerEnded - brokerStarted) / 1000.0,
                    brokerLimitChanges);

                lifecycleRuntime_.RecordResumeIfNeeded(
                    phaseInstrumented, lifecycleStage, phaseBinding);

                auto foregroundComponent = lifecycleRuntime_.ForegroundComponent(
                    packageFullName, foregroundProcess.processId, foregroundMemory,
                    foregroundHeartbeat, foregroundCpuObserved);
                auto brokerComponent = lifecycleRuntime_.BrokerComponent(
                    brokerFullName, brokerProcessId, describeReply.activationId,
                    brokerMemory, brokerHeartbeat, brokerCpuObserved);

                JsonObject foregroundIdentity;
                PutNumber(foregroundIdentity, L"process_id", foregroundProcess.processId);
                PutString(foregroundIdentity, L"executable_file_name", foregroundProcess.executableFileName);
                PutString(foregroundIdentity, L"application_id", foregroundProcess.applicationId);

                JsonObject foregroundAllocation;
                PutNumber(foregroundAllocation, L"requested_bytes", workerTouchedBytes);
                PutNumber(foregroundAllocation, L"committed_bytes", foregroundCommittedBytes);
                PutNumber(foregroundAllocation, L"touched_page_coverage_bytes", foregroundCommittedBytes);
                PutNumber(foregroundAllocation, L"page_bytes", foregroundHold.PageBytes());
                PutNumber(foregroundAllocation, L"page_count", foregroundHold.PageCount());
                PutString(foregroundAllocation, L"digest_algorithm", L"sha256");
                PutString(foregroundAllocation, L"digest_scope", L"one_mutable_verified_byte_per_committed_page");
                PutString(foregroundAllocation, L"digest_sha256", foregroundDigest);
                PutNumber(foregroundAllocation, L"verification_mismatch_count", foregroundHold.MismatchCount());
                PutBool(foregroundAllocation, L"release_succeeded", true);
                PutBool(foregroundAllocation, L"virtual_query_succeeded", foregroundHold.QuerySucceeded());
                PutNumber(foregroundAllocation, L"virtual_query_region_bytes", foregroundHold.QueryRegionBytes());

                JsonObject foreground;
                foreground.SetNamedValue(L"component", foregroundComponent);
                foreground.SetNamedValue(
                    L"workload_witness",
                    workloadSampler.Projection());
                foreground.SetNamedValue(L"identity", foregroundIdentity);
                PutString(foreground, L"lifecycle_state", L"foreground");
                foreground.SetNamedValue(L"memory", foregroundMemory);
                foreground.SetNamedValue(L"heartbeat", foregroundHeartbeat);
                PutBool(foreground, L"cpu_report_available", foregroundCpuReportAvailable);
                PutNumber(foreground, L"cpu_delta_100ns", foregroundCpuDelta100ns);
                PutBool(foreground, L"cpu_scheduling_observed", foregroundCpuObserved);
                foreground.SetNamedValue(L"allocation", foregroundAllocation);
                foreground.SetNamedValue(L"memory_before", MemorySampleObject(memoryBefore));
                foreground.SetNamedValue(L"memory_held", MemorySampleObject(memoryHeld));
                foreground.SetNamedValue(L"memory_after_release", MemorySampleObject(memoryAfter));
                foreground.SetNamedValue(
                    L"heartbeat_detail",
                    foregroundHold.HeartbeatsJson());
                lifecycleRuntime_.AttachForegroundJournal(foreground);

                JsonObject brokerIdentity;
                PutNumber(brokerIdentity, L"process_id", brokerProcessId);
                PutString(brokerIdentity, L"activation_id", describeReply.activationId);
                PutString(brokerIdentity, L"package_family_name", brokerFamily);

                JsonObject broker;
                broker.SetNamedValue(L"component", brokerComponent);
                broker.SetNamedValue(L"identity", brokerIdentity);
                PutString(broker, L"lifecycle_state", L"background");
                broker.SetNamedValue(L"memory", brokerMemory);
                broker.SetNamedValue(L"heartbeat", brokerHeartbeat);
                PutBool(broker, L"cpu_report_available", brokerCpuReportAvailable);
                PutNumber(broker, L"cpu_delta_100ns", brokerCpuDelta100ns);
                PutBool(broker, L"cpu_scheduling_observed", brokerCpuObserved);
                PutBool(
                    broker,
                    L"phase_instrumented",
                    phaseInstrumented);
                if (phaseInstrumented)
                {
                    broker.SetNamedValue(
                        L"phase_begin_result",
                        phaseBeginResult);
                    broker.SetNamedValue(
                        L"phase_complete_result",
                        phaseCompleteResult);
                    broker.SetNamedValue(
                        L"phase_complete_idempotent_result",
                        phaseCompleteReplayResult);
                }
                else
                {
                    PutNull(broker, L"phase_begin_result");
                    PutNull(broker, L"phase_complete_result");
                    PutNull(
                        broker,
                        L"phase_complete_idempotent_result");
                }
                if (storageVisibility)
                {
                    broker.SetNamedValue(L"storage_visibility", storageVisibility);
                }
                else
                {
                    PutNull(broker, L"storage_visibility");
                }
                broker.SetNamedValue(
                    L"describe_result",
                    describeReply.result);
                WorkerProcessTopologyLifecycleRuntime::AttachBrokerJournal(
                    broker, lifecycleSnapshot.journal);
                broker.SetNamedValue(
                    L"phase_status_before",
                    phaseStatusBefore.raw);
                broker.SetNamedValue(
                    L"phase_status_after",
                    phaseStatusAfter.raw);
                broker.SetNamedValue(L"last_metrics", lastBrokerMetrics);
                broker.SetNamedValue(L"last_allocation", lastBrokerAllocation);
                broker.SetNamedValue(
                    L"first_release_result",
                    firstReleaseReply.result);
                broker.SetNamedValue(
                    L"idempotent_release_result",
                    secondReleaseReply.result);

                JsonObject packageObject;
                PutString(packageObject, L"name", packageId.Name().c_str());
                PutString(packageObject, L"full_name", packageFullName);
                PutString(packageObject, L"family_name", packageFamily);
                PutString(packageObject, L"version", VersionString(packageId.Version()));
                PutNumber(
                    packageObject,
                    L"architecture",
                    static_cast<uint64_t>(
                        packageId.Architecture()));

                JsonObject memoryRelation;
                PutString(memoryRelation, L"relation", L"indeterminate");
                PutNumber(
                    memoryRelation,
                    L"simultaneous_verified_touched_bytes",
                    foregroundCommittedBytes +
                        brokerCommittedBytes);
                PutNumber(memoryRelation, L"worker_app_limit_bytes", memoryHeld.appLimitBytes);
                PutNumber(memoryRelation, L"broker_app_limit_bytes", brokerAppLimit);
                PutNumber(
                    memoryRelation,
                    L"sum_component_limits_bytes",
                    memoryHeld.appLimitBytes +
                        brokerAppLimit);
                PutNumber(
                    memoryRelation,
                    L"sum_component_usage_bytes",
                    memoryHeld.appUsageBytes +
                        brokerAppUsage);
                PutNumber(memoryRelation, L"overlap_started_unix_milliseconds", overlapStarted);
                PutNumber(memoryRelation, L"overlap_ended_unix_milliseconds", overlapEnded);
                PutNumber(memoryRelation, L"overlap_duration_ms", overlapMilliseconds);
                PutBool(memoryRelation, L"aggregate_pressure_observed", false);
                PutBool(memoryRelation, L"independent_budget_proven", false);
                PutBool(memoryRelation, L"shared_budget_proven", false);
                PutBool(memoryRelation, L"sum_is_not_proof_of_independent_budget", true);

                auto ipcLatency = ComputeIpcLatencyStatistics(ipcLatencies);
                JsonObject latency;
                PutNumber(latency, L"samples", ipcLatency.samples);
                PutDouble(latency, L"minimum", ipcLatency.minimum);
                PutDouble(latency, L"median", ipcLatency.median);
                PutDouble(latency, L"p95", ipcLatency.p95);
                PutDouble(latency, L"average", ipcLatency.average);
                PutDouble(latency, L"maximum", ipcLatency.maximum);

                JsonObject ipc;
                PutBool(ipc, L"available", true);
                PutString(ipc, L"transport", L"same_package_app_service_valueset_json");
                PutDouble(ipc, L"activation_latency_ms", openMilliseconds);
                PutNumber(ipc, L"commands_attempted", ipcCommandsCompleted);
                PutNumber(ipc, L"commands_completed", ipcCommandsCompleted);
                PutNumber(ipc, L"rounds_attempted", ipcCommandsCompleted);
                PutNumber(ipc, L"rounds_completed", ipcCommandsCompleted);
                PutNumber(ipc, L"heartbeat_rounds_attempted", heartbeatCount);
                PutNumber(ipc, L"heartbeat_rounds_completed", brokerHeartbeatsObserved);
                PutNumber(ipc, L"payload_rounds_attempted", heartbeatCount);
                PutNumber(ipc, L"payload_rounds_completed", payloadRoundsCompleted);
                PutNumber(ipc, L"payload_bytes_per_round", ipcPayloadBytes);
                PutNumber(ipc, L"response_utf8_bytes", ipcResponseBytes);
                auto overlapSeconds =
                    static_cast<double>(overlapMilliseconds) / 1000.0;
                PutDouble(
                    ipc,
                    L"foreground_to_auxiliary_bytes_per_second",
                    static_cast<double>(
                        payloadRoundsCompleted * ipcPayloadBytes) /
                        overlapSeconds);
                PutDouble(
                    ipc,
                    L"auxiliary_to_foreground_bytes_per_second",
                    static_cast<double>(ipcResponseBytes) /
                        overlapSeconds);
                ipc.SetNamedValue(L"latency_ms", latency);
                PutNumber(ipc, L"timeouts", 0ull);
                PutNumber(ipc, L"disconnects", 0ull);
                PutNumber(ipc, L"duplicate_deliveries", 0ull);

                auto appServiceClosedEventObserved =
                    transport.ServiceClosedObserved();
                JsonObject lifecycle;
                PutBool(lifecycle, L"foreground_release_completed", true);
                PutBool(lifecycle, L"broker_release_completed", true);
                PutBool(lifecycle, L"client_close_invoked", true);
                PutBool(lifecycle, L"client_close_completed",
                    clientCloseCompleted);
                PutBool(lifecycle, L"app_service_closed_event_observed",
                    appServiceClosedEventObserved);
                PutBool(lifecycle, L"app_service_closed",
                    appServiceClosedEventObserved);
                PutBool(lifecycle, L"broker_activation_stable", true);
                PutBool(lifecycle, L"broker_release_idempotent", true);

                return SuccessEnvelope(
                    protocolVersion,
                    correlationId,
                    packageObject,
                    foreground,
                    broker,
                    memoryRelation,
                    ipc,
                    lifecycle,
                    storageVisibility);
            }
            catch (...)
            {
                if (brokerAllocationActive)
                {
                    TryReleaseBroker(
                        transport,
                        deadline.CleanupTimeout());
                }
                throw;
            }
        }
        catch (Error const& error)
        {
            return ErrorEnvelope(
                protocolVersion,
                correlationId,
                error);
        }
        catch (hresult_error const& error)
        {
            return ErrorEnvelope(
                protocolVersion,
                correlationId,
                Error{
                    L"topology.app_service_response_failed",
                    L"a packaged platform API failed during the topology probe",
                    std::to_wstring(error.code().value) });
        }
        catch (...)
        {
            return ErrorEnvelope(
                protocolVersion,
                correlationId,
                Error{
                    L"topology.app_service_response_failed",
                    L"an unexpected bounded topology probe failure occurred",
                    L"unhandled" });
        }
    }
}