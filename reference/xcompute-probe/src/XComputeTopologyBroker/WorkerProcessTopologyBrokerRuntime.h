#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include "WorkerPersistentComputeCoordinatorRuntime.h"
#include "WorkerProcessTopologyBrokerPhaseRuntime.h"
#include "../shared/XcpProcessTopologyLifecycleJournal.h"

namespace winrt::XComputeTopologyBroker::implementation
{
    struct BrokerMemoryDigest final
    {
        uint64_t fnv64 = 14695981039346656037ull;
        hstring sha256;
    };

    class WorkerProcessTopologyBrokerRuntime final
    {
    public:
        WorkerProcessTopologyBrokerRuntime();
        ~WorkerProcessTopologyBrokerRuntime();

        WorkerProcessTopologyBrokerRuntime(
            WorkerProcessTopologyBrokerRuntime const&) = delete;
        WorkerProcessTopologyBrokerRuntime& operator=(
            WorkerProcessTopologyBrokerRuntime const&) = delete;

        hstring const& ActivationId() const noexcept;
        Windows::Data::Json::JsonObject ExecuteCommand(
            hstring const& command,
            Windows::Data::Json::JsonObject const& request);
        void OnMemoryLimitChanging(
            uint64_t oldLimitBytes,
            uint64_t newLimitBytes);
        bool Release(std::wstring_view reason) noexcept;

    private:
        Windows::Data::Json::JsonObject DescribeLocked() const;
        Windows::Data::Json::JsonObject PhaseStatusLocked() const;
        Windows::Data::Json::JsonObject PhaseBeginLocked(
            Windows::Data::Json::JsonObject const& request);
        Windows::Data::Json::JsonObject PhaseCompleteLocked(
            Windows::Data::Json::JsonObject const& request);
        Windows::Data::Json::JsonObject AllocateTouchLocked(
            Windows::Data::Json::JsonObject const& request);
        Windows::Data::Json::JsonObject HeartbeatLocked(
            Windows::Data::Json::JsonObject const& request);
        Windows::Data::Json::JsonObject ReleaseLocked(
            std::wstring_view reason);
        Windows::Data::Json::JsonObject Payload(
            Windows::Data::Json::JsonObject const& request) const;
        Windows::Data::Json::JsonObject MetricsLocked() const;
        Windows::Data::Json::JsonObject AllocationLocked() const;

        BrokerMemoryDigest TouchAndDigestLocked(
            uint64_t generation);
        BrokerMemoryDigest VerifyAndDigestLocked() const;
        uint64_t CommittedBytesLocked() const;
        bool ReleaseAllocationLocked(
            std::wstring_view reason) noexcept;

        mutable std::mutex mutex_;
        XComputeShared::XcpProcessTopologyLifecycleJournal lifecycleJournal_;
        hstring activationId_;
        std::chrono::steady_clock::time_point activatedAt_{};
        WorkerProcessTopologyBrokerHeartbeat heartbeatStatus_;
        WorkerProcessTopologyBrokerPhaseRuntime phaseRuntime_;
        WorkerPersistentComputeCoordinatorRuntime coordinatorRuntime_;
        void* allocation_ = nullptr;
        uint64_t allocationBytes_ = 0;
        uint64_t pageSize_ = 0;
        uint64_t pageCount_ = 0;
        uint64_t allocationSeed_ = 0;
        uint64_t allocationGeneration_ = 0;
        uint64_t allocationDigest_ = 0;
        hstring allocationDigestSha256_;
        uint64_t heartbeatCount_ = 0;
        uint64_t limitEventCount_ = 0;
        uint64_t limitForcedReleaseCount_ = 0;
        uint64_t lastOldLimitBytes_ = 0;
        uint64_t lastNewLimitBytes_ = 0;
        uint64_t lastReleasedBytes_ = 0;
        std::wstring lastReleaseReason_ = L"none";
    };
}
