#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.Threading.h>

namespace winrt::XComputeTopologyBroker::implementation
{
    inline constexpr uint64_t
        BrokerAutonomousHeartbeatIntervalMilliseconds = 100;

    struct BrokerHeartbeatSnapshot final
    {
        bool active = false;
        uint64_t intervalMilliseconds =
            BrokerAutonomousHeartbeatIntervalMilliseconds;
        uint64_t sequence = 0;
        uint64_t startedMonotonicMilliseconds = 0;
        uint64_t lastTickMonotonicMilliseconds = 0;
        uint64_t observedMonotonicMilliseconds = 0;
        uint64_t maximumGapMilliseconds = 0;

        Windows::Data::Json::JsonObject ToJson() const;
    };

    class WorkerProcessTopologyBrokerHeartbeat final
    {
    public:
        explicit WorkerProcessTopologyBrokerHeartbeat(
            std::function<void(uint64_t)> persistedHeartbeat = {});
        ~WorkerProcessTopologyBrokerHeartbeat();

        WorkerProcessTopologyBrokerHeartbeat(
            WorkerProcessTopologyBrokerHeartbeat const&) = delete;
        WorkerProcessTopologyBrokerHeartbeat& operator=(
            WorkerProcessTopologyBrokerHeartbeat const&) = delete;

        BrokerHeartbeatSnapshot Snapshot() const;

    private:
        struct State;

        std::shared_ptr<State> state_;
        Windows::System::Threading::ThreadPoolTimer timer_{ nullptr };
    };
}