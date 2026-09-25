#pragma once

#include "BrokerTask.g.h"

#include "WorkerProcessTopologyBrokerProtocol.h"
#include "WorkerProcessTopologyBrokerRuntime.h"

namespace winrt::XComputeTopologyBroker::implementation
{
    struct BrokerTask : BrokerTaskT<BrokerTask>
    {
        BrokerTask();
        ~BrokerTask();

        void Run(
            Windows::ApplicationModel::Background::
                IBackgroundTaskInstance const& taskInstance);

    private:
        void OnCanceled(
            Windows::ApplicationModel::Background::
                IBackgroundTaskInstance const& sender,
            Windows::ApplicationModel::Background::
                BackgroundTaskCancellationReason reason);
        void OnServiceClosed(
            Windows::ApplicationModel::AppService::
                AppServiceConnection const& sender,
            Windows::ApplicationModel::AppService::
                AppServiceClosedEventArgs const& args);
        void OnRequestReceived(
            Windows::ApplicationModel::AppService::
                AppServiceConnection const& sender,
            Windows::ApplicationModel::AppService::
                AppServiceRequestReceivedEventArgs const& args);
        void OnMemoryLimitChanging(
            Windows::Foundation::IInspectable const& sender,
            Windows::System::
                AppMemoryUsageLimitChangingEventArgs const& args);

        fire_and_forget HandleRequestAsync(
            Windows::ApplicationModel::AppService::
                AppServiceRequestReceivedEventArgs args);
        bool TryBeginRequest() noexcept;
        void EndRequest() noexcept;
        void CompleteTaskDeferralIfReady() noexcept;
        void Shutdown(std::wstring_view reason) noexcept;

        WorkerProcessTopologyBrokerRuntime runtime_;
        WorkerProcessTopologyBrokerProtocol protocol_;

        std::mutex lifecycleMutex_;
        Windows::ApplicationModel::Background::
            IBackgroundTaskInstance taskInstance_{ nullptr };
        Windows::ApplicationModel::Background::
            BackgroundTaskDeferral deferral_{ nullptr };
        Windows::ApplicationModel::AppService::
            AppServiceConnection connection_{ nullptr };
        event_token canceledToken_{};
        event_token requestToken_{};
        event_token serviceClosedToken_{};
        event_token memoryLimitToken_{};
        bool canceledHandlerRegistered_ = false;
        bool requestHandlerRegistered_ = false;
        bool serviceClosedHandlerRegistered_ = false;
        bool memoryLimitHandlerRegistered_ = false;
        std::atomic<bool> shutdownStarted_{ false };
        bool initializationInProgress_ = false;
        bool shutdownCleanupComplete_ = false;
        uint32_t inFlightRequests_ = 0;
    };
}

namespace winrt::XComputeTopologyBroker::factory_implementation
{
    struct BrokerTask :
        BrokerTaskT<BrokerTask, implementation::BrokerTask>
    {
    };
}
