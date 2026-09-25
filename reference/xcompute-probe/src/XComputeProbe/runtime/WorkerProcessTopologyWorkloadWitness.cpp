#include "pch.h"
#include "WorkerProcessTopologyWorkloadWitness.h"
#include "WorkerProcessTopologyProtocol.h"

#include <algorithm>
#include <atomic>

namespace XComputeProbe
{
    using namespace ProcessTopology;

    namespace
    {
        std::atomic<uint64_t> g_transitionSequence{ 0 };
        std::atomic<uint64_t> g_cpuSpmdReferenceActive{ 0 };
        std::atomic<uint64_t> g_gpuVerifiedDifferentialActive{ 0 };

        std::atomic<uint64_t>& Counter(
            WorkerProcessTopologyWorkloadKind kind) noexcept
        {
            return kind == WorkerProcessTopologyWorkloadKind::CpuSpmdReference
                ? g_cpuSpmdReferenceActive
                : g_gpuVerifiedDifferentialActive;
        }
    }

    WorkerProcessTopologyWorkloadScope::WorkerProcessTopologyWorkloadScope(
        WorkerProcessTopologyWorkloadKind kind) noexcept :
        m_kind(kind)
    {
        if (m_kind != WorkerProcessTopologyWorkloadKind::None)
        {
            Counter(m_kind).fetch_add(1, std::memory_order_acq_rel);
            g_transitionSequence.fetch_add(1, std::memory_order_release);
        }
    }

    WorkerProcessTopologyWorkloadScope::~WorkerProcessTopologyWorkloadScope()
    {
        if (m_kind != WorkerProcessTopologyWorkloadKind::None)
        {
            Counter(m_kind).fetch_sub(1, std::memory_order_acq_rel);
            g_transitionSequence.fetch_add(1, std::memory_order_release);
        }
    }

    WorkerProcessTopologyWorkloadSnapshot
        WorkerReadProcessTopologyWorkloadSnapshot() noexcept
    {
        WorkerProcessTopologyWorkloadSnapshot snapshot;
        snapshot.transitionSequence =
            g_transitionSequence.load(std::memory_order_acquire);
        snapshot.cpuSpmdReferenceActive =
            g_cpuSpmdReferenceActive.load(std::memory_order_acquire);
        snapshot.gpuVerifiedDifferentialActive =
            g_gpuVerifiedDifferentialActive.load(std::memory_order_acquire);
        return snapshot;
    }

    WorkerProcessTopologyWorkloadSampler::
        WorkerProcessTopologyWorkloadSampler() noexcept
    {
        auto initial = WorkerReadProcessTopologyWorkloadSnapshot();
        m_transitionSequenceStart = initial.transitionSequence;
        m_transitionSequenceEnd = initial.transitionSequence;
    }

    void WorkerProcessTopologyWorkloadSampler::Sample() noexcept
    {
        auto snapshot = WorkerReadProcessTopologyWorkloadSnapshot();
        ++m_samplesObserved;
        m_cpuActiveSamples += snapshot.cpuSpmdReferenceActive > 0 ? 1 : 0;
        m_gpuActiveSamples += snapshot.gpuVerifiedDifferentialActive > 0 ? 1 : 0;
        m_simultaneousActiveSamples +=
            snapshot.cpuSpmdReferenceActive > 0 &&
            snapshot.gpuVerifiedDifferentialActive > 0 ? 1 : 0;
        m_maximumCpuActive = (std::max)(
            m_maximumCpuActive,
            snapshot.cpuSpmdReferenceActive);
        m_maximumGpuActive = (std::max)(
            m_maximumGpuActive,
            snapshot.gpuVerifiedDifferentialActive);
        m_transitionSequenceEnd = (std::max)(
            m_transitionSequenceEnd,
            snapshot.transitionSequence);
    }

    winrt::Windows::Data::Json::JsonObject
        WorkerProcessTopologyWorkloadSampler::Projection() const
    {
        using winrt::Windows::Data::Json::JsonObject;
        JsonObject value;
        PutString(value, L"schema_version",
            L"xcp-process-topology-workload-witness-v1");
        PutNumber(value, L"samples_observed", m_samplesObserved);
        PutNumber(value, L"cpu_active_samples", m_cpuActiveSamples);
        PutNumber(value, L"gpu_active_samples", m_gpuActiveSamples);
        PutNumber(value, L"simultaneous_active_samples",
            m_simultaneousActiveSamples);
        PutNumber(value, L"maximum_cpu_active", m_maximumCpuActive);
        PutNumber(value, L"maximum_gpu_active", m_maximumGpuActive);
        PutNumber(value, L"transition_sequence_start",
            m_transitionSequenceStart);
        PutNumber(value, L"transition_sequence_end",
            m_transitionSequenceEnd);
        PutBool(value, L"cpu_spmd_reference_running_during_probe",
            m_cpuActiveSamples > 0);
        PutBool(value, L"gpu_verified_differential_running_during_probe",
            m_gpuActiveSamples > 0);
        PutBool(value, L"simultaneous_running_during_probe",
            m_simultaneousActiveSamples > 0);
        return value;
    }
}
