#pragma once

#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    class WorkerSessionRuntime;

    struct WorkerProtocolError : std::exception
    {
        std::string code;
        std::string message;

        WorkerProtocolError(std::string codeValue, std::string messageValue) :
            code(std::move(codeValue)),
            message(std::move(messageValue))
        {
        }

        char const* what() const noexcept override
        {
            return message.c_str();
        }
    };

    struct WorkerProtocolDispatchCallbacks
    {
        std::function<std::wstring(
            winrt::Windows::Data::Json::JsonObject const&,
            std::filesystem::path const&)> openTrustedSession;
        std::function<std::wstring(
            winrt::Windows::Data::Json::JsonObject const&,
            std::filesystem::path const&)> executeAuthorizedCommand;
    };

    class WorkerProtocolBoundary
    {
    public:
        WorkerProtocolBoundary(
            std::wstring protocolVersion,
            WorkerSessionRuntime& sessionRuntime);

        static constexpr wchar_t const* SchemaVersion()
        {
            return L"worker-session-protocol-boundary-0.1";
        }

        static constexpr wchar_t const* NegotiationSchemaVersion()
        {
            return L"worker-protocol-negotiation-0.1";
        }

        static constexpr wchar_t const* SdkCompatibilitySchemaVersion()
        {
            return L"worker-sdk-compatibility-0.1";
        }

        std::wstring Dispatch(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root,
            WorkerProtocolDispatchCallbacks const& callbacks);

        void RequireBinaryAuthorized(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& command,
            bool refreshSession = true);

        std::wstring TranslateRequest(std::function<std::wstring()> const& executor) const;
        std::wstring TranslateCommand(
            std::wstring const& command,
            std::function<std::wstring()> const& executor) const;

        std::wstring ErrorResponse(
            std::wstring const& code,
            std::wstring const& message,
            std::wstring const& command = L"",
            std::wstring const& detailsJson = L"") const;

    private:
        std::wstring NegotiateProtocol(
            winrt::Windows::Data::Json::JsonObject const& request) const;

        std::wstring protocolVersion_;
        WorkerSessionRuntime& sessionRuntime_;
    };
}
