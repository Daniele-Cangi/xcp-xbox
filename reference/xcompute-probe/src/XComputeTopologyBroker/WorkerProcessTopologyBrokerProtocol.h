#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::XComputeTopologyBroker::implementation
{
    inline constexpr wchar_t const* BrokerRequestSchema =
        L"xcp-process-topology-broker-request-v1";
    inline constexpr wchar_t const* BrokerResponseSchema =
        L"xcp-process-topology-broker-response-v1";
    inline constexpr wchar_t const* BrokerGateId =
        L"DUAL_PROCESS_MEMORY_TOPOLOGY_PROBE_V1";
    inline constexpr wchar_t const* BrokerContractName =
        L"xcp.process.topology.probe.v1";
    inline constexpr wchar_t const* BrokerEntryPointName =
        L"XComputeTopologyBroker.BrokerTask";
    inline constexpr wchar_t const* BrokerRequestJsonKey =
        L"request_json";
    inline constexpr wchar_t const* BrokerResponseJsonKey =
        L"response_json";
    inline constexpr wchar_t const* BrokerDigestScope =
        L"page_marker_u64_le_in_page_order";
    inline constexpr uint64_t BrokerMaximumAllocationBytes =
        4ull * 1024ull * 1024ull * 1024ull;
    inline constexpr size_t BrokerMaximumRequestUtf8Bytes =
        48ull * 1024ull;
    inline constexpr size_t BrokerMaximumPayloadUtf8Bytes =
        24ull * 1024ull;
    inline constexpr uint64_t BrokerDefaultSeed =
        0x584350544f504f31ull;

    struct BrokerFailure final
    {
        hstring code;
        hstring detail;
    };

    [[noreturn]] void FailBrokerRequest(
        std::wstring_view code,
        std::wstring_view detail);

    void PutBrokerString(
        Windows::Data::Json::JsonObject const& value,
        wchar_t const* name,
        std::wstring_view item);
    void PutBrokerBoolean(
        Windows::Data::Json::JsonObject const& value,
        wchar_t const* name,
        bool item);
    void PutBrokerNumber(
        Windows::Data::Json::JsonObject const& value,
        wchar_t const* name,
        uint64_t item);
    uint64_t ReadBrokerIntegral(
        Windows::Data::Json::JsonObject const& request,
        wchar_t const* name,
        uint64_t fallback,
        bool required);
    bool ReadBrokerBoolean(
        Windows::Data::Json::JsonObject const& request,
        wchar_t const* name,
        bool fallback);

    class WorkerProcessTopologyBrokerRuntime;

    class WorkerProcessTopologyBrokerProtocol final
    {
    public:
        explicit WorkerProcessTopologyBrokerProtocol(
            WorkerProcessTopologyBrokerRuntime& runtime) noexcept;

        hstring ExecuteRequest(hstring const& requestJson);
        hstring FailureResponse(
            std::wstring_view command,
            std::wstring_view code,
            std::wstring_view detail) const;

    private:
        Windows::Data::Json::JsonObject ResponseEnvelope(
            std::wstring_view command,
            bool ok,
            std::wstring_view code) const;

        WorkerProcessTopologyBrokerRuntime& runtime_;
    };
}