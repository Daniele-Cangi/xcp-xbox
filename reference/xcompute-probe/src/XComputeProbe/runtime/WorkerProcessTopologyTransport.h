#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <winrt/Windows.ApplicationModel.AppService.h>

namespace XComputeProbe::ProcessTopology
{
    struct TopologyTransportOpenResult
    {
        bool succeeded = false;
        int32_t statusCode = 0;
        double roundTripMilliseconds = 0.0;
    };

    struct TopologyTransportExchange
    {
        winrt::hstring responseJson;
        uint64_t responseUtf8Bytes = 0;
        std::chrono::steady_clock::time_point sendStarted{};
    };

    class WorkerProcessTopologyTransport final
    {
    public:
        WorkerProcessTopologyTransport();
        ~WorkerProcessTopologyTransport();

        WorkerProcessTopologyTransport(
            WorkerProcessTopologyTransport const&) = delete;
        WorkerProcessTopologyTransport& operator=(
            WorkerProcessTopologyTransport const&) = delete;

        TopologyTransportOpenResult Open(
            std::wstring_view appServiceName,
            std::wstring_view packageFamilyName,
            std::chrono::milliseconds timeout);
        TopologyTransportExchange Exchange(
            winrt::hstring const& requestJson,
            std::chrono::milliseconds timeout);
        bool ServiceClosedObserved() const noexcept;
        bool Close() noexcept;

    private:
        void UnsubscribeServiceClosed() noexcept;

        winrt::Windows::ApplicationModel::AppService::AppServiceConnection
            connection_;
        std::shared_ptr<std::atomic<bool>> serviceClosedState_;
        winrt::event_token serviceClosedToken_{};
        bool subscribed_ = false;
        bool closeInvoked_ = false;
        bool closeCompleted_ = false;
    };
}
