#include "pch.h"
#include "WorkerProcessTopologyRefusalEvidence.h"

#include "WorkerProcessTopologyTransport.h"

#include <algorithm>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {
        uint64_t SafetyReserve(uint64_t limit)
        {
            constexpr uint64_t minimum = 16ull * 1024ull * 1024ull;
            constexpr uint64_t maximum = 256ull * 1024ull * 1024ull;
            return (std::max)(minimum, (std::min)(maximum, limit / 16ull));
        }

        void RequireStableActivation(
            BrokerReply const& reply,
            std::wstring const& expectedActivation)
        {
            if (reply.activationId != expectedActivation)
            {
                Fail(
                    L"topology.app_service_payload_invalid",
                    L"the broker activation changed while sealing allocation evidence");
            }
        }
    }

    BrokerReply AllocateBrokerWithRefusalEvidence(
        WorkerProcessTopologyTransport& transport,
        MonotonicDeadline const& deadline,
        WorkerProcessTopologyLifecycleRuntime const& lifecycleRuntime,
        MeasuredHold& foregroundHold,
        MemorySample const& memoryBefore,
        std::mutex& limitMutex,
        std::vector<LimitChange> const& limitChanges,
        std::wstring const& packageFullName,
        ProcessIdentity const& foregroundProcess,
        std::wstring const& brokerFullName,
        uint64_t brokerProcessId,
        std::wstring const& brokerActivationId,
        uint64_t workerTouchedBytes,
        uint64_t brokerTouchedBytes,
        uint64_t brokerCommittedBytes,
        uint64_t heartbeatCount,
        uint64_t holdIntervalMilliseconds)
    {
        auto allocateRequest = NewBrokerCommand(L"allocate_touch");
        PutNumber(allocateRequest, L"bytes", brokerCommittedBytes);
        PutNumber(allocateRequest, L"seed", 5843501ull);

        BrokerReply allocateReply;
        try
        {
            allocateReply = SendBrokerCommand(
                transport,
                allocateRequest,
                deadline.Remaining());
        }
        catch (Error const& error)
        {
            if (error.code != L"topology.memory_allocation_failed" ||
                error.detail != L"ALLOCATION_HEADROOM_LIMIT")
            {
                throw;
            }

            foregroundHold.Join();
            auto memoryHeld = CaptureMemorySample();
            uint64_t foregroundLimitEventCount = 0;
            {
                std::lock_guard<std::mutex> lock(limitMutex);
                foregroundLimitEventCount = limitChanges.size();
            }

            auto describeReply = SendBrokerCommand(
                transport,
                NewBrokerCommand(L"describe"),
                deadline.Remaining());
            RequireStableActivation(describeReply, brokerActivationId);
            auto brokerMetrics = RequiredObject(
                describeReply.result,
                L"metrics",
                L"topology.app_service_payload_invalid");
            if (RequiredUInt64(
                    brokerMetrics,
                    L"process_id",
                    L"topology.app_service_payload_invalid") != brokerProcessId)
            {
                Fail(
                    L"topology.app_service_payload_invalid",
                    L"the broker process changed while sealing allocation refusal evidence");
            }

            auto brokerAppLimit = RequiredUInt64(
                brokerMetrics,
                L"app_memory_usage_limit_bytes",
                L"topology.app_service_payload_invalid");
            auto brokerAppUsage = RequiredUInt64(
                brokerMetrics,
                L"app_memory_usage_bytes",
                L"topology.app_service_payload_invalid");
            auto safetyReserve = SafetyReserve(brokerAppLimit);
            if (brokerAppLimit == 0 || brokerAppUsage > brokerAppLimit ||
                brokerTouchedBytes + safetyReserve <=
                    brokerAppLimit - brokerAppUsage)
            {
                Fail(
                    L"topology.app_service_payload_invalid",
                    L"the broker refusal could not be reproduced from measured headroom");
            }

            auto firstReleaseReply = SendBrokerCommand(
                transport,
                NewBrokerCommand(L"release"),
                deadline.Remaining());
            RequireStableActivation(firstReleaseReply, brokerActivationId);
            auto secondReleaseReply = SendBrokerCommand(
                transport,
                NewBrokerCommand(L"release"),
                deadline.Remaining());
            RequireStableActivation(secondReleaseReply, brokerActivationId);
            if (!RequiredBool(
                    firstReleaseReply.result,
                    L"idempotent_success",
                    L"topology.app_service_payload_invalid") ||
                !RequiredBool(
                    secondReleaseReply.result,
                    L"idempotent_success",
                    L"topology.app_service_payload_invalid"))
            {
                Fail(
                    L"topology.memory_verification_failed",
                    L"the refused broker allocation did not release idempotently");
            }

            auto foregroundCommittedBytes = foregroundHold.CommittedBytes();
            auto foregroundDigest = foregroundHold.DigestSha256();
            auto foregroundStarted = foregroundHold.StartedUnixMilliseconds();
            auto foregroundEnded = foregroundHold.EndedUnixMilliseconds();
            if (!foregroundHold.Release())
            {
                Fail(
                    L"topology.memory_verification_failed",
                    L"the foreground allocation was not released after broker refusal");
            }
            if (!transport.Close())
            {
                Fail(
                    L"topology.app_service_response_failed",
                    L"the broker transport did not close after allocation refusal");
            }

            auto foregroundCpuObserved =
                memoryBefore.cpuReportAvailable &&
                memoryHeld.cpuReportAvailable &&
                memoryHeld.cpuUserTime100ns >= memoryBefore.cpuUserTime100ns &&
                memoryHeld.cpuKernelTime100ns >= memoryBefore.cpuKernelTime100ns &&
                ((memoryHeld.cpuUserTime100ns - memoryBefore.cpuUserTime100ns) +
                    (memoryHeld.cpuKernelTime100ns - memoryBefore.cpuKernelTime100ns)) > 0;
            auto foregroundHeartbeat = HeartbeatProjection(
                holdIntervalMilliseconds,
                heartbeatCount,
                foregroundHold.HeartbeatCount(),
                foregroundHold.MaximumGapMilliseconds(),
                true);
            auto brokerHeartbeat = HeartbeatProjection(
                BrokerAutonomousHeartbeatIntervalMilliseconds,
                0,
                0,
                0,
                false);
            auto foregroundMemory = MemoryProjection(
                memoryHeld.appLimitBytes,
                memoryHeld.appUsageBytes,
                memoryHeld.privatePageCountBytes,
                memoryHeld.workingSetBytes,
                foregroundCommittedBytes,
                workerTouchedBytes,
                foregroundCommittedBytes,
                foregroundDigest,
                foregroundHold.HeartbeatCount(),
                static_cast<double>(foregroundEnded - foregroundStarted) / 1000.0,
                foregroundLimitEventCount);
            auto brokerMemory = MemoryProjection(
                brokerAppLimit,
                brokerAppUsage,
                NestedUInt64(
                    brokerMetrics,
                    L"process_diagnostics_memory",
                    L"private_page_bytes"),
                NestedUInt64(
                    brokerMetrics,
                    L"process_diagnostics_memory",
                    L"working_set_bytes"),
                0,
                brokerTouchedBytes,
                0,
                L"",
                0,
                0.0,
                OptionalUInt64(brokerMetrics, L"memory_limit_event_count"));
            auto foregroundComponent = lifecycleRuntime.ForegroundComponent(
                packageFullName,
                foregroundProcess.processId,
                foregroundMemory,
                foregroundHeartbeat,
                foregroundCpuObserved);
            auto brokerComponent =
                WorkerProcessTopologyLifecycleRuntime::BrokerComponent(
                    brokerFullName,
                    brokerProcessId,
                    brokerActivationId,
                    brokerMemory,
                    brokerHeartbeat,
                    false);

            JsonObject foregroundEvidence;
            foregroundEvidence.SetNamedValue(L"component", foregroundComponent);
            foregroundEvidence.SetNamedValue(L"memory", foregroundMemory);
            foregroundEvidence.SetNamedValue(L"heartbeat", foregroundHeartbeat);
            JsonObject brokerEvidence;
            brokerEvidence.SetNamedValue(L"component", brokerComponent);
            brokerEvidence.SetNamedValue(L"memory", brokerMemory);
            brokerEvidence.SetNamedValue(L"heartbeat", brokerHeartbeat);

            JsonObject refusalEvidence;
            PutString(
                refusalEvidence,
                L"schema_version",
                L"xcp-process-topology-memory-refusal-v1");
            PutString(refusalEvidence, L"boundary", L"memory_pressure");
            PutString(refusalEvidence, L"refusal_component", L"broker");
            PutString(refusalEvidence, L"failure_code", error.code);
            PutString(refusalEvidence, L"failure_detail", error.detail);
            PutNumber(refusalEvidence, L"safety_reserve_bytes", safetyReserve);
            refusalEvidence.SetNamedValue(L"foreground", foregroundEvidence);
            refusalEvidence.SetNamedValue(L"broker", brokerEvidence);
            PutBool(refusalEvidence, L"foreground_release_completed", true);
            PutBool(refusalEvidence, L"broker_release_completed", true);
            PutBool(refusalEvidence, L"client_close_completed", true);

            Error structuredError = error;
            structuredError.refusalEvidence = refusalEvidence;
            throw structuredError;
        }

        RequireStableActivation(allocateReply, brokerActivationId);
        auto allocated = RequiredObject(
            allocateReply.result,
            L"allocation",
            L"topology.app_service_payload_invalid");
        if (!RequiredBool(
                allocated,
                L"active",
                L"topology.app_service_payload_invalid") ||
            RequiredUInt64(
                allocated,
                L"committed_bytes",
                L"topology.app_service_payload_invalid") != brokerCommittedBytes ||
            !IsLowerHex(
                RequiredString(
                    allocated,
                    L"digest_sha256",
                    L"topology.app_service_payload_invalid"),
                64))
        {
            Fail(
                L"topology.memory_allocation_failed",
                L"the broker allocation was not committed, touched, and verified");
        }
        return allocateReply;
    }
}
