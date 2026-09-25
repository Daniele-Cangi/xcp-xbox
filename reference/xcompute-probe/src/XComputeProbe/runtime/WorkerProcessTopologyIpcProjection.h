#pragma once

#include <cstdint>
#include <vector>

namespace XComputeProbe::ProcessTopology
{
    struct IpcLatencyStatistics final
    {
        uint64_t samples{};
        double minimum{};
        double median{};
        double p95{};
        double average{};
        double maximum{};
    };

    IpcLatencyStatistics ComputeIpcLatencyStatistics(
        std::vector<double> const& latencies);
}