#include "pch.h"
#include "WorkerProtocolBoundary.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>
#include <vector>

#include "WorkerArtifactManifestCompute.h"
#include "WorkerArtifactPublicationRuntime.h"
#include "WorkerCpuCapsuleRuntime.h"
#include "WorkerArtifactStore.h"
#include "WorkerCreativeInstallRuntime.h"
#include "WorkerGraphExecution.h"
#include "WorkerGraphResourceLedger.h"
#include "WorkerGraphValidation.h"
#include "WorkerSessionRuntime.h"
#include "WorkerTaskPlanRuntime.h"
#include "WorkerXvmTypes.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        std::wstring Utf8ToWide(std::string const& value)
        {
            return winrt::to_hstring(value).c_str();
        }

        std::wstring GetOptionalString(
            JsonObject const& object,
            wchar_t const* name,
            std::wstring const& fallback = L"")
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedString(name, fallback);
            return std::wstring(value.data(), value.size());
        }

        wchar_t const* BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        bool TryParseProtocolVersion(
            std::wstring const& value,
            std::pair<uint64_t, uint64_t>& parsed)
        {
            auto separator = value.find(L'.');
            if (separator == std::wstring::npos ||
                separator == 0 ||
                separator + 1 >= value.size() ||
                value.find(L'.', separator + 1) != std::wstring::npos)
            {
                return false;
            }

            auto majorText = value.substr(0, separator);
            auto minorText = value.substr(separator + 1);
            auto digitsOnly = [](std::wstring const& text)
            {
                return !text.empty() &&
                    std::all_of(
                        text.begin(),
                        text.end(),
                        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
            };
            if (!digitsOnly(majorText) || !digitsOnly(minorText))
            {
                return false;
            }

            try
            {
                parsed = {
                    static_cast<uint64_t>(std::stoull(majorText)),
                    static_cast<uint64_t>(std::stoull(minorText))
                };
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        template <typename TError>
        std::wstring CodedErrorResponse(
            WorkerProtocolBoundary const& boundary,
            TError const& error,
            std::wstring const& command)
        {
            return boundary.ErrorResponse(
                Utf8ToWide(error.code),
                Utf8ToWide(error.message),
                command);
        }

        std::wstring XvmCodedErrorResponse(
            WorkerProtocolBoundary const& boundary,
            WorkerXvmError const& error,
            std::wstring const& command)
        {
            auto const& details = error.details;
            auto detailsJson =
                L"{\"schema_version\":\"xvm-error-details-v1\"" +
                std::wstring(L",\"stage\":") +
                JsonString(Utf8ToWide(details.stage)) +
                L",\"field\":" + JsonString(Utf8ToWide(details.field)) +
                L",\"expected\":" + JsonString(Utf8ToWide(details.expected)) +
                L",\"actual\":" + JsonString(Utf8ToWide(details.actual)) +
                L",\"correction\":" + JsonString(Utf8ToWide(details.correction)) +
                L",\"retryable\":" + BoolJson(details.retryable) +
                L"}";
            return boundary.ErrorResponse(
                Utf8ToWide(error.code),
                Utf8ToWide(error.message),
                command,
                detailsJson);
        }

        std::wstring CreativeCodedErrorResponse(
            WorkerProtocolBoundary const& boundary,
            WorkerCreativeHostError const& error,
            std::wstring const& command)
        {
            auto const& details = error.details;
            auto detailsJson =
                L"{\"schema_version\":\"xcp-creative-error-details-v1\"" +
                std::wstring(L",\"stage\":") +
                JsonString(Utf8ToWide(details.stage)) +
                L",\"field\":" + JsonString(Utf8ToWide(details.field)) +
                L",\"path\":" + JsonString(Utf8ToWide(details.path)) +
                L",\"expected\":" + JsonString(Utf8ToWide(details.expected)) +
                L",\"actual\":" + JsonString(Utf8ToWide(details.actual)) +
                L",\"correction\":" + JsonString(Utf8ToWide(details.correction)) +
                L",\"retryable\":" + BoolJson(details.retryable) +
                L"}";
            return boundary.ErrorResponse(
                Utf8ToWide(error.code),
                Utf8ToWide(error.message),
                command,
                detailsJson);
        }
    }

    WorkerProtocolBoundary::WorkerProtocolBoundary(
        std::wstring protocolVersion,
        WorkerSessionRuntime& sessionRuntime) :
        protocolVersion_(std::move(protocolVersion)),
        sessionRuntime_(sessionRuntime)
    {
    }

    std::wstring WorkerProtocolBoundary::ErrorResponse(
        std::wstring const& code,
        std::wstring const& message,
        std::wstring const& command,
        std::wstring const& detailsJson) const
    {
        std::wstring response =
            L"{\"ok\":false,\"protocol_version\":" +
            JsonString(protocolVersion_);
        if (!command.empty())
        {
            response += L",\"command\":" + JsonString(command);
        }
        response +=
            L",\"error\":{\"code\":" + JsonString(code) +
            L",\"message\":" + JsonString(message);
        if (!detailsJson.empty())
        {
            response += L",\"details\":" + detailsJson;
        }
        response += L"}}";
        return response;
    }

    std::wstring WorkerProtocolBoundary::TranslateCommand(
        std::wstring const& command,
        std::function<std::wstring()> const& executor) const
    {
        try
        {
            return executor();
        }
        catch (WorkerProtocolError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerCpuCapsuleError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerArtifactManifestComputeError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerArtifactPublicationError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerArtifactStoreError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerCreativeHostError const& error)
        {
            return CreativeCodedErrorResponse(
                *this,
                error,
                command);
        }
        catch (WorkerGraphValidationError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerGraphExecutionError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerGraphResourceError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerTaskPlanError const& error)
        {
            return CodedErrorResponse(*this, error, command);
        }
        catch (WorkerXvmError const& error)
        {
            return XvmCodedErrorResponse(*this, error, command);
        }
        catch (hresult_error const& error)
        {
            return ErrorResponse(
                L"command.hresult",
                error.message().c_str(),
                command);
        }
        catch (std::exception const& error)
        {
            return ErrorResponse(
                L"command.exception",
                Utf8ToWide(error.what()),
                command);
        }
        catch (...)
        {
            return ErrorResponse(
                L"command.unhandled",
                L"unhandled command error",
                command);
        }
    }

    std::wstring WorkerProtocolBoundary::TranslateRequest(
        std::function<std::wstring()> const& executor) const
    {
        try
        {
            return executor();
        }
        catch (WorkerProtocolError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerCpuCapsuleError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerArtifactManifestComputeError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerArtifactPublicationError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerArtifactStoreError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerCreativeHostError const& error)
        {
            return CreativeCodedErrorResponse(
                *this,
                error,
                L"");
        }
        catch (WorkerGraphValidationError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerGraphExecutionError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerGraphResourceError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerTaskPlanError const& error)
        {
            return CodedErrorResponse(*this, error, L"");
        }
        catch (WorkerXvmError const& error)
        {
            return XvmCodedErrorResponse(*this, error, L"");
        }
        catch (hresult_error const& error)
        {
            return ErrorResponse(
                L"json.invalid",
                error.message().c_str());
        }
        catch (std::exception const& error)
        {
            return ErrorResponse(
                L"command.exception",
                Utf8ToWide(error.what()));
        }
        catch (...)
        {
            return ErrorResponse(
                L"command.unhandled",
                L"unhandled command processing error");
        }
    }

    std::wstring WorkerProtocolBoundary::NegotiateProtocol(
        JsonObject const& request) const
    {
        auto sdkMin = GetOptionalString(request, L"sdk_protocol_min");
        auto sdkMax = GetOptionalString(request, L"sdk_protocol_max");
        if (sdkMin.empty())
        {
            throw WorkerProtocolError(
                "protocol.sdk_min_required",
                "sdk_protocol_min is required");
        }
        if (sdkMax.empty())
        {
            throw WorkerProtocolError(
                "protocol.sdk_max_required",
                "sdk_protocol_max is required");
        }

        std::pair<uint64_t, uint64_t> sdkMinParsed;
        std::pair<uint64_t, uint64_t> sdkMaxParsed;
        std::pair<uint64_t, uint64_t> workerParsed;
        if (!TryParseProtocolVersion(sdkMin, sdkMinParsed) ||
            !TryParseProtocolVersion(sdkMax, sdkMaxParsed) ||
            !TryParseProtocolVersion(protocolVersion_, workerParsed))
        {
            throw WorkerProtocolError(
                "protocol.version_invalid",
                "protocol versions must use major.minor decimal form");
        }
        if (sdkMinParsed > sdkMaxParsed)
        {
            throw WorkerProtocolError(
                "protocol.range_invalid",
                "sdk_protocol_min cannot exceed sdk_protocol_max");
        }

        auto compatible =
            sdkMinParsed <= workerParsed &&
            workerParsed <= sdkMaxParsed;
        auto sdkContractVersion = GetOptionalString(
            request,
            L"sdk_contract_version",
            L"unspecified");

        return L"{\"ok\":true,\"protocol_version\":" +
            JsonString(protocolVersion_) +
            L",\"command\":\"negotiate_protocol\"" +
            L",\"schema_version\":" +
            JsonString(NegotiationSchemaVersion()) +
            L",\"sdk_compatibility_schema\":" +
            JsonString(SdkCompatibilitySchemaVersion()) +
            L",\"worker_protocol_min\":" +
            JsonString(protocolVersion_) +
            L",\"worker_protocol_max\":" +
            JsonString(protocolVersion_) +
            L",\"sdk_protocol_min\":" + JsonString(sdkMin) +
            L",\"sdk_protocol_max\":" + JsonString(sdkMax) +
            L",\"sdk_contract_version\":" +
            JsonString(sdkContractVersion) +
            L",\"compatible\":" + BoolJson(compatible) +
            L",\"compatibility_mode\":" +
            JsonString(compatible ? L"declared_range" : L"incompatible") +
            L",\"auth_required_for_worker_commands\":true" +
            L"}";
    }

    std::wstring WorkerProtocolBoundary::Dispatch(
        JsonObject const& request,
        std::filesystem::path const& root,
        WorkerProtocolDispatchCallbacks const& callbacks)
    {
        auto command = GetOptionalString(request, L"command");

        if (command == L"negotiate_protocol")
        {
            return NegotiateProtocol(request);
        }
        if (command == L"open_session")
        {
            return sessionRuntime_.OpenSession(request);
        }
        if (command == L"open_session_with_trust")
        {
            if (!callbacks.openTrustedSession)
            {
                throw WorkerProtocolError(
                    "command.unhandled",
                    "trusted-session callback is unavailable");
            }
            return callbacks.openTrustedSession(request, root);
        }
        if (command == L"session_status")
        {
            return sessionRuntime_.SessionStatus(request);
        }
        if (command == L"close_session")
        {
            return sessionRuntime_.CloseSession(request);
        }

        auto isBatch = request.HasKey(L"commands");
        auto routedRequest = request;
        auto authStarted = std::chrono::steady_clock::now();
        sessionRuntime_.RequireAuthorized(
            routedRequest,
            isBatch ? L"batch" : command);
        auto authElapsed = std::chrono::steady_clock::now() - authStarted;
        auto authMs = std::chrono::duration<double, std::milli>(
            authElapsed).count();
        routedRequest.Insert(
            L"_server_auth_ms",
            JsonValue::CreateNumberValue(authMs));

        if (!callbacks.executeAuthorizedCommand)
        {
            throw WorkerProtocolError(
                "command.unhandled",
                "authorized-command callback is unavailable");
        }

        if (!isBatch)
        {
            return callbacks.executeAuthorizedCommand(routedRequest, root);
        }

        auto commands = routedRequest.GetNamedArray(L"commands");
        std::vector<std::wstring> results;
        bool allOk = true;
        for (uint32_t i = 0; i < commands.Size(); ++i)
        {
            auto value = commands.GetAt(i);
            if (value.ValueType() != JsonValueType::Object)
            {
                results.push_back(ErrorResponse(
                    L"command.invalid",
                    L"commands entries must be objects"));
                allOk = false;
                continue;
            }

            auto commandObject = value.GetObject();
            auto commandName = GetOptionalString(
                commandObject,
                L"command");
            auto response = TranslateCommand(
                commandName,
                [&]()
                {
                    return callbacks.executeAuthorizedCommand(
                        commandObject,
                        root);
                });
            if (response.find(L"\"ok\":false") != std::wstring::npos)
            {
                allOk = false;
            }
            results.push_back(std::move(response));
        }

        std::wostringstream out;
        out << L"{\"ok\":" << BoolJson(allOk)
            << L",\"protocol_version\":" << JsonString(protocolVersion_)
            << L",\"batch\":true,\"result_count\":"
            << results.size() << L",\"results\":[";
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (i != 0)
            {
                out << L",";
            }
            out << results[i];
        }
        out << L"]}";
        return out.str();
    }

    void WorkerProtocolBoundary::RequireBinaryAuthorized(
        JsonObject const& request,
        std::wstring const& command,
        bool refreshSession)
    {
        static std::vector<std::wstring> const supported{
            L"write_chunk_binary",
            L"write_chunks_stream",
            L"append_artifact_chunk_binary",
            L"read_chunk_binary",
            L"read_chunks_stream",
            L"read_artifact_chunk_binary"
        };
        if (std::find(
                supported.begin(),
                supported.end(),
                command) == supported.end())
        {
            throw WorkerProtocolError(
                "command.binary_unsupported",
                "binary payload or response is not supported for this command");
        }
        sessionRuntime_.RequireAuthorized(
            request,
            command,
            refreshSession);
    }
}
