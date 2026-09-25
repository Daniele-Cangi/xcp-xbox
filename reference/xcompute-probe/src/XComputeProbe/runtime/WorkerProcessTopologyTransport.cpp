#include "pch.h"
#include "WorkerProcessTopologyTransport.h"

#include "WorkerProcessTopologyProtocol.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <future>
#include <memory>

using namespace winrt;
using namespace Windows::ApplicationModel::AppService;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {
        double ElapsedMilliseconds(
            std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }

        template <typename TOperation>
        void CancelNoThrow(TOperation const& operation) noexcept
        {
            try
            {
                operation.Cancel();
            }
            catch (...)
            {
            }
        }

        template <typename TResult>
        TResult AwaitAsyncOperation(
            IAsyncOperation<TResult> const& operation,
            std::chrono::milliseconds timeout,
            wchar_t const* failureCode,
            wchar_t const* failureMessage)
        {
            if (timeout.count() <= 0)
            {
                CancelNoThrow(operation);
                Fail(
                    L"topology.app_service_timeout",
                    L"the topology App Service operation had no remaining deadline",
                    failureMessage);
            }

            auto completion =
                std::make_shared<std::promise<void>>();
            auto completed = completion->get_future();
            try
            {
                operation.Completed(
                    [completion](
                        IAsyncOperation<TResult> const&,
                        AsyncStatus) noexcept
                    {
                        try
                        {
                            completion->set_value();
                        }
                        catch (...)
                        {
                        }
                    });
            }
            catch (hresult_error const& error)
            {
                Fail(
                    failureCode,
                    failureMessage,
                    std::to_wstring(error.code().value));
            }

            if (completed.wait_for(timeout) !=
                std::future_status::ready)
            {
                CancelNoThrow(operation);
                Fail(
                    L"topology.app_service_timeout",
                    L"the topology App Service operation exceeded the monotonic deadline",
                    failureMessage);
            }
            try
            {
                return operation.GetResults();
            }
            catch (hresult_error const& error)
            {
                Fail(
                    failureCode,
                    failureMessage,
                    std::to_wstring(error.code().value));
            }
        }
    }

    WorkerProcessTopologyTransport::WorkerProcessTopologyTransport()
        : serviceClosedState_(
            std::make_shared<std::atomic<bool>>(false))
    {
    }

    WorkerProcessTopologyTransport::~WorkerProcessTopologyTransport()
    {
        Close();
        UnsubscribeServiceClosed();
    }

    TopologyTransportOpenResult WorkerProcessTopologyTransport::Open(
        std::wstring_view appServiceName,
        std::wstring_view packageFamilyName,
        std::chrono::milliseconds timeout)
    {
        connection_.AppServiceName(hstring(appServiceName));
        connection_.PackageFamilyName(hstring(packageFamilyName));
        auto closedState = serviceClosedState_;
        serviceClosedToken_ = connection_.ServiceClosed(
            [closedState](
                AppServiceConnection const&,
                AppServiceClosedEventArgs const&) noexcept
            {
                closedState->store(true);
            });
        subscribed_ = true;

        auto started = std::chrono::steady_clock::now();
        IAsyncOperation<AppServiceConnectionStatus> operation{ nullptr };
        try
        {
            operation = connection_.OpenAsync();
        }
        catch (hresult_error const& error)
        {
            Fail(
                L"topology.app_service_open_failed",
                L"the same-package topology App Service open failed",
                std::to_wstring(error.code().value));
        }
        auto status = AwaitAsyncOperation(
            operation,
            timeout,
            L"topology.app_service_open_failed",
            L"the same-package topology App Service open failed");
        return TopologyTransportOpenResult{
            status == AppServiceConnectionStatus::Success,
            static_cast<int32_t>(status),
            ElapsedMilliseconds(started) };
    }

    TopologyTransportExchange WorkerProcessTopologyTransport::Exchange(
        hstring const& requestJson,
        std::chrono::milliseconds timeout)
    {
        ValueSet message;
        message.Insert(
            L"request_json",
            box_value(requestJson));

        auto started = std::chrono::steady_clock::now();
        IAsyncOperation<AppServiceResponse> operation{ nullptr };
        try
        {
            operation = connection_.SendMessageAsync(message);
        }
        catch (hresult_error const& error)
        {
            Fail(
                L"topology.app_service_response_failed",
                L"the topology broker request transport failed",
                std::to_wstring(error.code().value));
        }
        auto response = AwaitAsyncOperation(
            operation,
            timeout,
            L"topology.app_service_response_failed",
            L"the topology broker request transport failed");
        if (!response ||
            response.Status() != AppServiceResponseStatus::Success)
        {
            Fail(
                L"topology.app_service_response_failed",
                L"the topology broker did not return a successful response",
                response
                    ? std::to_wstring(
                        static_cast<int32_t>(response.Status()))
                    : L"null_response");
        }

        auto values = response.Message();
        if (!values || !values.HasKey(L"response_json"))
        {
            Fail(
                L"topology.app_service_payload_invalid",
                L"the broker response_json ValueSet field is absent");
        }
        auto responseText = unbox_value_or<hstring>(
            values.Lookup(L"response_json"),
            hstring{});
        auto responseBytes = to_string(responseText).size();
        if (responseText.empty() ||
            responseBytes > 1024ull * 1024ull)
        {
            Fail(
                L"topology.app_service_payload_invalid",
                L"the broker response_json is empty or outside its bound");
        }

        return TopologyTransportExchange{
            responseText,
            static_cast<uint64_t>(responseBytes),
            started };
    }

    bool WorkerProcessTopologyTransport::ServiceClosedObserved() const noexcept
    {
        return serviceClosedState_->load();
    }

    bool WorkerProcessTopologyTransport::Close() noexcept
    {
        if (closeInvoked_)
        {
            return closeCompleted_;
        }
        closeInvoked_ = true;
        try
        {
            connection_.Close();
            closeCompleted_ = true;
        }
        catch (...)
        {
            closeCompleted_ = false;
        }
        return closeCompleted_;
    }

    void WorkerProcessTopologyTransport::UnsubscribeServiceClosed() noexcept
    {
        if (!subscribed_)
        {
            return;
        }
        try
        {
            connection_.ServiceClosed(serviceClosedToken_);
        }
        catch (...)
        {
        }
        subscribed_ = false;
    }
}
