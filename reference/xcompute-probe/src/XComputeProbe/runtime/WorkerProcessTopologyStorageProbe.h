#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <winrt/Windows.Data.Json.h>
#include "WorkerProcessTopologyProtocol.h"

namespace XComputeProbe::ProcessTopology
{
    class WorkerProcessTopologyTransport;
    struct WorkerProcessTopologyStorageProbeResult final
    {
        winrt::Windows::Data::Json::JsonObject measurement{ nullptr };
        std::vector<BrokerReply> brokerReplies;
    };
    class WorkerProcessTopologyStorageProbe final
    {
    public:
        static WorkerProcessTopologyStorageProbeResult Run(
            WorkerProcessTopologyTransport& transport,
            std::wstring const& phaseRunId,
            uint64_t payloadBytes,
            MonotonicDeadline const& deadline);
    };
}