#include "pch.h"
#include "BrokerTask.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::AppService;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::System;

namespace winrt::XComputeTopologyBroker::implementation
{
    BrokerTask::BrokerTask()
        : protocol_(runtime_)
    {
    }

    BrokerTask::~BrokerTask()
    {
        Shutdown(L"destructor");
    }

    void BrokerTask::Run(
        IBackgroundTaskInstance const& taskInstance)
    {
        auto taskDeferral = taskInstance.GetDeferral();
        bool initialize = false;
        {
            std::lock_guard guard(lifecycleMutex_);
            if (!shutdownStarted_.load(
                    std::memory_order_acquire))
            {
                initializationInProgress_ = true;
                taskInstance_ = taskInstance;
                deferral_ = taskDeferral;
                initialize = true;
            }
        }
        if (!initialize)
        {
            try
            {
                taskDeferral.Complete();
            }
            catch (...)
            {
            }
            return;
        }

        auto finish = [&]() noexcept
        {
            try
            {
                std::lock_guard guard(lifecycleMutex_);
                initializationInProgress_ = false;
            }
            catch (...)
            {
            }
            CompleteTaskDeferralIfReady();
        };
        auto stop = [&](std::wstring_view reason) noexcept
        {
            Shutdown(reason);
            finish();
        };
        auto publish = [&](
            event_token token,
            event_token& target,
            bool& registered,
            auto&& unregister)
        {
            bool keep = false;
            {
                std::lock_guard guard(lifecycleMutex_);
                if (!shutdownStarted_.load(
                        std::memory_order_acquire))
                {
                    target = token;
                    registered = true;
                    keep = true;
                }
            }
            if (!keep)
            {
                try
                {
                    unregister(token);
                }
                catch (...)
                {
                }
            }
            return keep;
        };

        try
        {
            auto canceled = taskInstance.Canceled(
                { this, &BrokerTask::OnCanceled });
            if (!publish(
                    canceled,
                    canceledToken_,
                    canceledHandlerRegistered_,
                    [&](event_token token)
                    {
                        taskInstance.Canceled(token);
                    }))
            {
                finish();
                return;
            }

            auto details =
                taskInstance.TriggerDetails().try_as<
                    AppServiceTriggerDetails>();
            if (!details)
            {
                stop(L"invalid_trigger_details");
                return;
            }
            if (details.Name() != BrokerContractName)
            {
                stop(L"unexpected_app_service_name");
                return;
            }
            if (
                details.CallerPackageFamilyName() !=
                Package::Current().Id().FamilyName())
            {
                stop(
                    L"unexpected_caller_package_family");
                return;
            }
            if (details.IsRemoteSystemConnection())
            {
                stop(L"remote_system_connection_forbidden");
                return;
            }
            auto connection =
                details.AppServiceConnection();
            if (!connection)
            {
                stop(L"missing_app_service_connection");
                return;
            }

            bool keepConnection = false;
            {
                std::lock_guard guard(lifecycleMutex_);
                if (!shutdownStarted_.load(
                        std::memory_order_acquire))
                {
                    connection_ = connection;
                    keepConnection = true;
                }
            }
            if (!keepConnection)
            {
                finish();
                return;
            }

            auto memoryLimit =
                MemoryManager::AppMemoryUsageLimitChanging(
                    {
                        this,
                        &BrokerTask::
                            OnMemoryLimitChanging
                    });
            if (!publish(
                    memoryLimit,
                    memoryLimitToken_,
                    memoryLimitHandlerRegistered_,
                    [&](event_token token)
                    {
                        MemoryManager::
                            AppMemoryUsageLimitChanging(
                                token);
                    }))
            {
                finish();
                return;
            }

            auto serviceClosed =
                connection.ServiceClosed(
                    {
                        this,
                        &BrokerTask::OnServiceClosed
                    });
            if (!publish(
                    serviceClosed,
                    serviceClosedToken_,
                    serviceClosedHandlerRegistered_,
                    [&](event_token token)
                    {
                        connection.ServiceClosed(token);
                    }))
            {
                finish();
                return;
            }

            auto request =
                connection.RequestReceived(
                    {
                        this,
                        &BrokerTask::OnRequestReceived
                    });
            if (!publish(
                    request,
                    requestToken_,
                    requestHandlerRegistered_,
                    [&](event_token token)
                    {
                        connection.RequestReceived(token);
                    }))
            {
                finish();
                return;
            }
        }
        catch (...)
        {
            stop(L"app_service_initialization_failed");
            return;
        }
        finish();
    }

    void BrokerTask::OnCanceled(
        IBackgroundTaskInstance const&,
        BackgroundTaskCancellationReason)
    {
        Shutdown(L"background_task_canceled");
    }

    void BrokerTask::OnServiceClosed(
        AppServiceConnection const&,
        AppServiceClosedEventArgs const&)
    {
        Shutdown(L"app_service_closed");
    }

    void BrokerTask::OnRequestReceived(
        AppServiceConnection const&,
        AppServiceRequestReceivedEventArgs const& args)
    {
        HandleRequestAsync(args);
    }

    fire_and_forget BrokerTask::HandleRequestAsync(
        AppServiceRequestReceivedEventArgs args)
    {
        auto lifetime = get_strong();
        (void)lifetime;

        bool requestAdmitted = TryBeginRequest();
        AppServiceDeferral requestDeferral{ nullptr };
        try
        {
            requestDeferral = args.GetDeferral();
        }
        catch (...)
        {
            if (requestAdmitted)
            {
                EndRequest();
            }
            co_return;
        }

        hstring responseJson;
        try
        {
            co_await resume_background();
            if (!requestAdmitted)
            {
                responseJson = protocol_.FailureResponse(
                    L"unknown",
                    L"SHUTTING_DOWN",
                    L"the broker no longer admits requests for this activation");
            }
            else
            {
                auto message =
                    args.Request().Message();
                auto boxed = message.TryLookup(
                    BrokerRequestJsonKey);
                if (!boxed)
                {
                    boxed = message.TryLookup(L"request");
                }
                auto requestJson =
                    unbox_value_or<hstring>(
                        boxed,
                        hstring{});
                if (requestJson.empty())
                {
                    responseJson =
                        protocol_.FailureResponse(
                            L"unknown",
                            L"MISSING_REQUEST_JSON",
                            L"ValueSet must contain a non-empty request_json string");
                }
                else
                {
                    responseJson =
                        protocol_.ExecuteRequest(
                            requestJson);
                }
            }
        }
        catch (hresult_error const& error)
        {
            responseJson = protocol_.FailureResponse(
                L"unknown",
                L"REQUEST_TRANSPORT_ERROR",
                std::wstring_view(error.message()));
        }
        catch (...)
        {
            responseJson = protocol_.FailureResponse(
                L"unknown",
                L"REQUEST_TRANSPORT_ERROR",
                L"unexpected request transport failure");
        }

        try
        {
            ValueSet reply;
            reply.Insert(
                BrokerResponseJsonKey,
                box_value(responseJson));
            co_await args.Request().SendResponseAsync(
                reply);
        }
        catch (...)
        {
        }
        try
        {
            requestDeferral.Complete();
        }
        catch (...)
        {
        }
        if (requestAdmitted)
        {
            EndRequest();
        }
    }

    void BrokerTask::OnMemoryLimitChanging(
        IInspectable const&,
        AppMemoryUsageLimitChangingEventArgs const& args)
    {
        runtime_.OnMemoryLimitChanging(
            args.OldLimit(),
            args.NewLimit());
    }

    bool BrokerTask::TryBeginRequest() noexcept
    {
        try
        {
            std::lock_guard guard(lifecycleMutex_);
            if (shutdownStarted_.load(
                    std::memory_order_acquire))
            {
                return false;
            }
            ++inFlightRequests_;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void BrokerTask::EndRequest() noexcept
    {
        try
        {
            std::lock_guard guard(lifecycleMutex_);
            if (inFlightRequests_ != 0)
            {
                --inFlightRequests_;
            }
        }
        catch (...)
        {
            return;
        }
        CompleteTaskDeferralIfReady();
    }

    void BrokerTask::CompleteTaskDeferralIfReady() noexcept
    {
        BackgroundTaskDeferral taskDeferral{ nullptr };
        try
        {
            std::lock_guard guard(lifecycleMutex_);
            if (
                !shutdownStarted_.load(
                    std::memory_order_acquire) ||
                initializationInProgress_ ||
                !shutdownCleanupComplete_ ||
                inFlightRequests_ != 0 ||
                !deferral_)
            {
                return;
            }
            taskDeferral = deferral_;
            deferral_ = nullptr;
        }
        catch (...)
        {
            return;
        }
        try
        {
            taskDeferral.Complete();
        }
        catch (...)
        {
        }
    }

    void BrokerTask::Shutdown(
        std::wstring_view reason) noexcept
    {
        if (shutdownStarted_.exchange(
                true,
                std::memory_order_acq_rel))
        {
            return;
        }

        AppServiceConnection connection{ nullptr };
        IBackgroundTaskInstance taskInstance{ nullptr };
        event_token canceled{};
        event_token request{};
        event_token serviceClosed{};
        event_token memoryLimit{};
        bool removeCanceled = false;
        bool removeRequest = false;
        bool removeServiceClosed = false;
        bool removeMemoryLimit = false;
        try
        {
            std::lock_guard guard(lifecycleMutex_);
            connection = connection_;
            taskInstance = taskInstance_;
            canceled = canceledToken_;
            request = requestToken_;
            serviceClosed = serviceClosedToken_;
            memoryLimit = memoryLimitToken_;
            removeCanceled = canceledHandlerRegistered_;
            removeRequest = requestHandlerRegistered_;
            removeServiceClosed =
                serviceClosedHandlerRegistered_;
            removeMemoryLimit =
                memoryLimitHandlerRegistered_;
            canceledHandlerRegistered_ = false;
            requestHandlerRegistered_ = false;
            serviceClosedHandlerRegistered_ = false;
            memoryLimitHandlerRegistered_ = false;
            connection_ = nullptr;
            taskInstance_ = nullptr;
        }
        catch (...)
        {
        }

        if (removeMemoryLimit)
        {
            try
            {
                MemoryManager::
                    AppMemoryUsageLimitChanging(
                        memoryLimit);
            }
            catch (...)
            {
            }
        }
        if (connection && removeRequest)
        {
            try
            {
                connection.RequestReceived(request);
            }
            catch (...)
            {
            }
        }
        if (connection && removeServiceClosed)
        {
            try
            {
                connection.ServiceClosed(serviceClosed);
            }
            catch (...)
            {
            }
        }
        if (taskInstance && removeCanceled)
        {
            try
            {
                taskInstance.Canceled(canceled);
            }
            catch (...)
            {
            }
        }

        runtime_.Release(reason);
        try
        {
            std::lock_guard guard(lifecycleMutex_);
            shutdownCleanupComplete_ = true;
        }
        catch (...)
        {
        }
        CompleteTaskDeferralIfReady();
    }
}
