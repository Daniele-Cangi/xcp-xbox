#pragma once

#include <cstdint>
#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    enum class WorkerProcessTopologyWorkloadKind
    {
        None,
        CpuSpmdReference,
        GpuVerifiedDifferential,
    };

    struct WorkerProcessTopologyWorkloadSnapshot
    {
        uint64_t transitionSequence = 0;
        uint64_t cpuSpmdReferenceActive = 0;
        uint64_t gpuVerifiedDifferentialActive = 0;
    };

    class WorkerProcessTopologyWorkloadScope final
    {
    public:
        explicit WorkerProcessTopologyWorkloadScope(
            WorkerProcessTopologyWorkloadKind kind) noexcept;
        ~WorkerProcessTopologyWorkloadScope();

        WorkerProcessTopologyWorkloadScope(
            WorkerProcessTopologyWorkloadScope const&) = delete;
        WorkerProcessTopologyWorkloadScope& operator=(
            WorkerProcessTopologyWorkloadScope const&) = delete;

    private:
        WorkerProcessTopologyWorkloadKind m_kind;
    };

    WorkerProcessTopologyWorkloadSnapshot
        WorkerReadProcessTopologyWorkloadSnapshot() noexcept;

    class WorkerProcessTopologyWorkloadSampler final
    {
    public:
        WorkerProcessTopologyWorkloadSampler() noexcept;
        void Sample() noexcept;
        winrt::Windows::Data::Json::JsonObject Projection() const;

    private:
        uint64_t m_samplesObserved = 0;
        uint64_t m_cpuActiveSamples = 0;
        uint64_t m_gpuActiveSamples = 0;
        uint64_t m_simultaneousActiveSamples = 0;
        uint64_t m_maximumCpuActive = 0;
        uint64_t m_maximumGpuActive = 0;
        uint64_t m_transitionSequenceStart = 0;
        uint64_t m_transitionSequenceEnd = 0;
    };
}
