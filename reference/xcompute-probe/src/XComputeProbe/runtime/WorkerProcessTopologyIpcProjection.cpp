#include "pch.h"
#include "WorkerProcessTopologyIpcProjection.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace XComputeProbe::ProcessTopology
{
    IpcLatencyStatistics ComputeIpcLatencyStatistics(
        std::vector<double> const& latencies)
    {
        if (latencies.empty())
        {
            throw std::invalid_argument("IPC latency sample set cannot be empty");
        }
        auto sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        auto count = sorted.size();
        auto median = count % 2 == 0
            ? (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0
            : sorted[count / 2];
        auto p95Index = (std::max)(
            static_cast<size_t>(1),
            static_cast<size_t>(std::ceil(0.95 * static_cast<double>(count)))) - 1;
        double total = 0.0;
        for (auto value : sorted)
        {
            total += value;
        }
        return {
            static_cast<uint64_t>(count),
            sorted.front(),
            median,
            sorted[p95Index],
            total / static_cast<double>(count),
            sorted.back()
        };
    }
}