#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "WorkerProcessTopologyLifecycleRuntime.h"
#include "WorkerProcessTopologyMemoryProbe.h"
#include "WorkerProcessTopologyProtocol.h"

namespace XComputeProbe::ProcessTopology
{
    class WorkerProcessTopologyTransport;

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
        uint64_t holdIntervalMilliseconds);
}
