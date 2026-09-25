#include "pch.h"
#include "WorkerProcessTopologyBrokerHeartbeat.h"

#include "WorkerProcessTopologyBrokerProtocol.h"

#include <chrono>
#include <mutex>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::System::Threading;

namespace winrt::XComputeTopologyBroker::implementation
{
    namespace
    {
        uint64_t MonotonicMilliseconds() noexcept
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    JsonObject BrokerHeartbeatSnapshot::ToJson() const
    {
        JsonObject heartbeat;
        PutBrokerString(
            heartbeat,
            L"source",
            L"BROKER_PERIODIC_THREAD_POOL_TIMER");
        PutBrokerBoolean(heartbeat, L"active", active);
        PutBrokerNumber(heartbeat, L"interval_ms", intervalMilliseconds);
        PutBrokerNumber(heartbeat, L"sequence", sequence);
        PutBrokerNumber(
            heartbeat,
            L"started_monotonic_ms",
            startedMonotonicMilliseconds);
        PutBrokerNumber(
            heartbeat,
            L"last_tick_monotonic_ms",
            lastTickMonotonicMilliseconds);
        PutBrokerNumber(
            heartbeat,
            L"observed_monotonic_ms",
            observedMonotonicMilliseconds);
        PutBrokerNumber(
            heartbeat,
            L"maximum_gap_ms",
            maximumGapMilliseconds);
        return heartbeat;
    }

    struct WorkerProcessTopologyBrokerHeartbeat::State final
    {
        mutable std::mutex mutex;
        bool active = true;
        uint64_t sequence = 0;
        uint64_t startedMonotonicMilliseconds = 0;
        uint64_t lastTickMonotonicMilliseconds = 0;
        uint64_t maximumGapMilliseconds = 0;
        std::function<void(uint64_t)> persistedHeartbeat;
    };

    WorkerProcessTopologyBrokerHeartbeat::
        WorkerProcessTopologyBrokerHeartbeat(
            std::function<void(uint64_t)> persistedHeartbeat)
        : state_(std::make_shared<State>())
    {
        state_->persistedHeartbeat = std::move(persistedHeartbeat);
        auto started = MonotonicMilliseconds();
        state_->startedMonotonicMilliseconds = started;
        state_->lastTickMonotonicMilliseconds = started;
        auto period = std::chrono::duration_cast<TimeSpan>(
            std::chrono::milliseconds(
                BrokerAutonomousHeartbeatIntervalMilliseconds));
        auto state = state_;
        timer_ = ThreadPoolTimer::CreatePeriodicTimer(
            [state](ThreadPoolTimer const&) noexcept
            {
                try
                {
                    auto now = MonotonicMilliseconds();
                    std::function<void(uint64_t)> persistedHeartbeat;
                    uint64_t sequence = 0;
                    {
                        std::lock_guard guard(state->mutex);
                        if (!state->active)
                        {
                            return;
                        }
                        auto gap = now >= state->lastTickMonotonicMilliseconds
                            ? now - state->lastTickMonotonicMilliseconds
                            : 0;
                        state->maximumGapMilliseconds = (std::max)(
                            state->maximumGapMilliseconds,
                            gap);
                        state->lastTickMonotonicMilliseconds = now;
                        sequence = ++state->sequence;
                        if (sequence != 0)
                        {
                            persistedHeartbeat = state->persistedHeartbeat;
                        }
                    }
                    if (persistedHeartbeat)
                    {
                        persistedHeartbeat(sequence);
                    }
                }
                catch (...)
                {
                }
            },
            period);
    }

    WorkerProcessTopologyBrokerHeartbeat::
        ~WorkerProcessTopologyBrokerHeartbeat()
    {
        try
        {
            std::lock_guard guard(state_->mutex);
            state_->active = false;
        }
        catch (...)
        {
        }
        try
        {
            if (timer_)
            {
                timer_.Cancel();
                timer_ = nullptr;
            }
        }
        catch (...)
        {
        }
    }

    BrokerHeartbeatSnapshot
        WorkerProcessTopologyBrokerHeartbeat::Snapshot() const
    {
        BrokerHeartbeatSnapshot snapshot;
        {
            std::lock_guard guard(state_->mutex);
            snapshot.active = state_->active;
            snapshot.sequence = state_->sequence;
            snapshot.startedMonotonicMilliseconds =
                state_->startedMonotonicMilliseconds;
            snapshot.lastTickMonotonicMilliseconds =
                state_->lastTickMonotonicMilliseconds;
            snapshot.maximumGapMilliseconds =
                state_->maximumGapMilliseconds;
        }
        snapshot.observedMonotonicMilliseconds =
            MonotonicMilliseconds();
        return snapshot;
    }
}