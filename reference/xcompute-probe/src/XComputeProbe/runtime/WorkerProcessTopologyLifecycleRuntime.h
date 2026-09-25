#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <winrt/Windows.Data.Json.h>

#include "WorkerProcessTopologyProtocol.h"
#include "../../shared/XcpProcessTopologyLifecycleJournal.h"

namespace XComputeProbe::ProcessTopology
{
    class WorkerProcessTopologyTransport;

    struct WorkerProcessTopologyLifecycleBrokerSnapshot
    {
        winrt::Windows::Data::Json::JsonObject journal{ nullptr };
        double roundTripMilliseconds = 0.0;
        uint64_t responseUtf8Bytes = 0;
    };

    class WorkerProcessTopologyLifecycleRuntime final
    {
    public:
        WorkerProcessTopologyLifecycleRuntime();

        static bool IsPhaseRequestSchema(
            std::wstring_view schemaVersion) noexcept;
        std::wstring ValidateLifecycleStage(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring_view requestSchema,
            BrokerPhaseBinding const& phaseBinding) const;
        void RecordLifecycleEvent(std::wstring_view eventName) noexcept;
        void RecordResumeIfNeeded(
            bool phaseInstrumented,
            std::wstring_view lifecycleStage,
            BrokerPhaseBinding const& phaseBinding) noexcept;
        WorkerProcessTopologyLifecycleBrokerSnapshot CaptureBrokerSnapshot(
            WorkerProcessTopologyTransport& transport,
            MonotonicDeadline const& deadline,
            std::wstring const& expectedActivationId) const;
        winrt::Windows::Data::Json::JsonObject ForegroundComponent(
            std::wstring const& packageIdentity,
            uint64_t processId,
            winrt::Windows::Data::Json::JsonObject const& memory,
            winrt::Windows::Data::Json::JsonObject const& heartbeat,
            bool cpuSchedulingObserved) const;
        static winrt::Windows::Data::Json::JsonObject BrokerComponent(
            std::wstring const& packageIdentity,
            uint64_t processId,
            std::wstring const& activationIdentity,
            winrt::Windows::Data::Json::JsonObject const& memory,
            winrt::Windows::Data::Json::JsonObject const& heartbeat,
            bool cpuSchedulingObserved);
        void AttachForegroundJournal(
            winrt::Windows::Data::Json::JsonObject const& foreground) const;
        static void AttachBrokerJournal(
            winrt::Windows::Data::Json::JsonObject const& broker,
            winrt::Windows::Data::Json::JsonObject const& journal);

    private:
        static bool IsLifecyclePhase(std::wstring_view phaseId) noexcept;
        XComputeShared::XcpProcessTopologyLifecycleJournal journal_;
    };
}
