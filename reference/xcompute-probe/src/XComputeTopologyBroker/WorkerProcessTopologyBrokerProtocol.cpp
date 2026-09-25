#include "pch.h"
#include "WorkerProcessTopologyBrokerProtocol.h"

#include "WorkerProcessTopologyBrokerRuntime.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace winrt::XComputeTopologyBroker::implementation
{
    [[noreturn]] void FailBrokerRequest(
        std::wstring_view code,
        std::wstring_view detail)
    {
        throw BrokerFailure{ hstring(code), hstring(detail) };
    }

    void PutBrokerString(
        JsonObject const& value,
        wchar_t const* name,
        std::wstring_view item)
    {
        value.SetNamedValue(
            name,
            JsonValue::CreateStringValue(hstring(item)));
    }

    void PutBrokerBoolean(
        JsonObject const& value,
        wchar_t const* name,
        bool item)
    {
        value.SetNamedValue(
            name,
            JsonValue::CreateBooleanValue(item));
    }

    void PutBrokerNumber(
        JsonObject const& value,
        wchar_t const* name,
        uint64_t item)
    {
        value.SetNamedValue(
            name,
            JsonValue::CreateNumberValue(
                static_cast<double>(item)));
    }

    uint64_t ReadBrokerIntegral(
        JsonObject const& request,
        wchar_t const* name,
        uint64_t fallback,
        bool required)
    {
        if (!request.HasKey(name))
        {
            if (required)
            {
                FailBrokerRequest(
                    L"MISSING_ARGUMENT",
                    std::wstring(L"missing field: ") + name);
            }
            return fallback;
        }

        double number = 0.0;
        try
        {
            number = request.GetNamedNumber(name);
        }
        catch (...)
        {
            FailBrokerRequest(
                L"INVALID_ARGUMENT_TYPE",
                std::wstring(
                    L"field must be a JSON number: ") + name);
        }
        if (!std::isfinite(number) ||
            number < 0.0 ||
            std::floor(number) != number ||
            number > 9007199254740991.0)
        {
            FailBrokerRequest(
                L"INVALID_ARGUMENT_RANGE",
                std::wstring(
                    L"field is outside the exact unsigned integer range: ") +
                    name);
        }
        return static_cast<uint64_t>(number);
    }

    bool ReadBrokerBoolean(
        JsonObject const& request,
        wchar_t const* name,
        bool fallback)
    {
        if (!request.HasKey(name))
        {
            return fallback;
        }
        try
        {
            return request.GetNamedBoolean(name);
        }
        catch (...)
        {
            FailBrokerRequest(
                L"INVALID_ARGUMENT_TYPE",
                std::wstring(
                    L"field must be a JSON boolean: ") + name);
        }
    }

    WorkerProcessTopologyBrokerProtocol::
        WorkerProcessTopologyBrokerProtocol(
            WorkerProcessTopologyBrokerRuntime& runtime) noexcept
        : runtime_(runtime)
    {
    }

    JsonObject WorkerProcessTopologyBrokerProtocol::ResponseEnvelope(
        std::wstring_view command,
        bool ok,
        std::wstring_view code) const
    {
        JsonObject response;
        PutBrokerString(
            response,
            L"schema_version",
            BrokerResponseSchema);
        PutBrokerString(response, L"gate_id", BrokerGateId);
        PutBrokerString(
            response,
            L"protocol",
            BrokerContractName);
        PutBrokerString(
            response,
            L"app_service_name",
            BrokerContractName);
        PutBrokerString(
            response,
            L"resource_group",
            BrokerContractName);
        PutBrokerString(
            response,
            L"entry_point",
            BrokerEntryPointName);
        PutBrokerString(
            response,
            L"activation_id",
            std::wstring_view(runtime_.ActivationId()));
        PutBrokerString(response, L"command", command);
        PutBrokerBoolean(response, L"ok", ok);
        PutBrokerString(response, L"code", code);
        return response;
    }

    hstring WorkerProcessTopologyBrokerProtocol::FailureResponse(
        std::wstring_view command,
        std::wstring_view code,
        std::wstring_view detail) const
    {
        auto response = ResponseEnvelope(
            command,
            false,
            code);
        PutBrokerString(response, L"detail", detail);
        return response.Stringify();
    }

    hstring WorkerProcessTopologyBrokerProtocol::ExecuteRequest(
        hstring const& requestJson)
    {
        hstring command = L"unknown";
        try
        {
            if (to_string(requestJson).size() >
                BrokerMaximumRequestUtf8Bytes)
            {
                FailBrokerRequest(
                    L"REQUEST_TOO_LARGE",
                    L"request_json exceeds the broker transport bound");
            }

            JsonObject request{ nullptr };
            try
            {
                request = JsonObject::Parse(requestJson);
            }
            catch (...)
            {
                FailBrokerRequest(
                    L"INVALID_JSON",
                    L"request_json is not a valid JSON object");
            }

            hstring schemaVersion;
            try
            {
                if (!request.HasKey(L"schema_version"))
                {
                    FailBrokerRequest(
                        L"UNSUPPORTED_SCHEMA",
                        L"request.schema_version is required");
                }
                schemaVersion =
                    request.GetNamedString(L"schema_version");
            }
            catch (BrokerFailure const&)
            {
                throw;
            }
            catch (...)
            {
                FailBrokerRequest(
                    L"UNSUPPORTED_SCHEMA",
                    L"request.schema_version must be a string");
            }
            if (schemaVersion != BrokerRequestSchema)
            {
                FailBrokerRequest(
                    L"UNSUPPORTED_SCHEMA",
                    L"request.schema_version is not supported by this broker");
            }
            if (!request.HasKey(L"command"))
            {
                FailBrokerRequest(
                    L"MISSING_COMMAND",
                    L"request.command is required");
            }
            try
            {
                command = request.GetNamedString(L"command");
            }
            catch (...)
            {
                FailBrokerRequest(
                    L"INVALID_COMMAND",
                    L"request.command must be a string");
            }

            auto result =
                runtime_.ExecuteCommand(command, request);
            auto response = ResponseEnvelope(
                std::wstring_view(command),
                true,
                L"OK");
            response.SetNamedValue(L"result", result);
            return response.Stringify();
        }
        catch (BrokerFailure const& failure)
        {
            return FailureResponse(
                std::wstring_view(command),
                std::wstring_view(failure.code),
                std::wstring_view(failure.detail));
        }
        catch (hresult_error const& error)
        {
            return FailureResponse(
                std::wstring_view(command),
                L"WINRT_FAILURE",
                std::wstring_view(error.message()));
        }
        catch (...)
        {
            return FailureResponse(
                std::wstring_view(command),
                L"INTERNAL_FAILURE",
                L"unexpected broker failure");
        }
    }
}