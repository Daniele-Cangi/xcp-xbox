#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <winrt/Windows.Data.Json.h>

#include "WorkerProcessTopologyPhaseProtocol.h"

namespace XComputeProbe::ProcessTopology
{
    class WorkerProcessTopologyTransport;

    inline constexpr wchar_t const* GateId =
        L"DUAL_PROCESS_MEMORY_TOPOLOGY_PROBE_V1";
    inline constexpr wchar_t const* CapabilitySchema =
        L"xcp-process-topology-capability-v1";
    inline constexpr wchar_t const* RequestSchema =
        L"xcp-process-topology-probe-v1-runtime-request";
    inline constexpr wchar_t const* ResultSchema =
        L"xcp-process-topology-probe-v1-runtime-result";
    inline constexpr wchar_t const* BrokerRequestSchema =
        L"xcp-process-topology-broker-request-v1";
    inline constexpr wchar_t const* BrokerResponseSchema =
        L"xcp-process-topology-broker-response-v1";
    inline constexpr wchar_t const* AppServiceName =
        L"xcp.process.topology.probe.v1";

    inline constexpr uint64_t DefaultTouchedBytes = 8ull * 1024ull * 1024ull;
    inline constexpr uint64_t MaximumTouchedBytes = 512ull * 1024ull * 1024ull;
    inline constexpr uint64_t MaximumHeartbeatCount = 1024;
    inline constexpr uint64_t MaximumHoldIntervalMilliseconds = 1000;
    inline constexpr uint64_t MaximumHeartbeatWindowMilliseconds = 30000;
    inline constexpr uint64_t MaximumPayloadBytes = 24ull * 1024ull;
    inline constexpr uint64_t MaximumStorageProbeBytes = 1024ull * 1024ull;
    inline constexpr uint64_t MaximumOperationDeadlineMilliseconds = 30000;
    inline constexpr uint64_t MaximumCleanupTimeoutMilliseconds = 1000;
    inline constexpr uint64_t BrokerAutonomousHeartbeatIntervalMilliseconds = 100;
    inline constexpr uint64_t MaximumAggregateTouchedRoundBytes =
        8ull * 1024ull * 1024ull * 1024ull;
    inline constexpr uint64_t MaximumAggregatePayloadRoundBytes =
        16ull * 1024ull * 1024ull;

    struct Error
    {
        std::wstring code;
        std::wstring message;
        std::wstring detail;
        winrt::Windows::Data::Json::JsonObject refusalEvidence{ nullptr };
    };

    [[noreturn]] void Fail(
        std::wstring const& code,
        std::wstring const& message,
        std::wstring const& detail = L"");

    class MonotonicDeadline
    {
    public:
        MonotonicDeadline() noexcept
            : expires_(
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(
                    MaximumOperationDeadlineMilliseconds))
        {
        }

        std::chrono::milliseconds Remaining() const
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= expires_)
            {
                Fail(
                    L"topology.app_service_timeout",
                    L"the topology probe exhausted its monotonic deadline");
            }
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    expires_ - now);
            if (remaining.count() <= 0)
            {
                Fail(
                    L"topology.app_service_timeout",
                    L"the topology probe has less than one millisecond remaining");
            }
            return remaining;
        }

        std::chrono::milliseconds CleanupTimeout() const noexcept
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= expires_)
            {
                return std::chrono::milliseconds(0);
            }
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    expires_ - now);
            auto maximum = std::chrono::milliseconds(
                MaximumCleanupTimeoutMilliseconds);
            return remaining < maximum ? remaining : maximum;
        }

        void RequireMeasuredWindow(
            uint64_t heartbeatCount,
            uint64_t holdIntervalMilliseconds) const
        {
            constexpr uint64_t reserveMilliseconds = 1000;
            auto remaining = static_cast<uint64_t>(
                Remaining().count());
            auto requested =
                heartbeatCount * holdIntervalMilliseconds;
            if (remaining <= reserveMilliseconds ||
                requested > remaining - reserveMilliseconds)
            {
                Fail(
                    L"topology.app_service_timeout",
                    L"the requested measured window cannot fit inside the remaining topology deadline");
            }
        }

        void Sleep(uint64_t milliseconds) const
        {
            if (milliseconds >=
                static_cast<uint64_t>(Remaining().count()))
            {
                Fail(
                    L"topology.app_service_timeout",
                    L"the topology hold interval exceeds the remaining monotonic deadline");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(milliseconds));
        }

    private:
        std::chrono::steady_clock::time_point expires_;
    };

    bool IsSafeToken(std::wstring const& value, size_t maximumLength);
    bool IsLowerHex(std::wstring const& value, size_t length);

    std::wstring RequiredString(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode);
    bool RequiredBool(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode);
    uint64_t RequiredUInt64(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode);
    uint64_t OptionalUInt64(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        uint64_t fallback = 0);
    winrt::Windows::Data::Json::JsonObject RequiredObject(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode);
    winrt::Windows::Data::Json::JsonObject OptionalObject(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name);
    uint64_t NestedUInt64(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* child,
        wchar_t const* name,
        uint64_t fallback = 0);

    void PutString(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring_view value);
    void PutNumber(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        uint64_t value);
    void PutDouble(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        double value);
    void PutBool(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        bool value);
    void PutNull(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name);

    winrt::Windows::Data::Json::JsonArray StableErrorCodes();
    winrt::Windows::Data::Json::JsonObject ClaimBoundary();
    winrt::Windows::Data::Json::JsonObject ErrorCodes(
        std::wstring const& observed);
    std::wstring ErrorEnvelope(
        std::wstring const& protocolVersion,
        std::wstring const& correlationId,
        Error const& error);

    winrt::Windows::Data::Json::JsonObject NewBrokerCommand(
        std::wstring_view command);

    struct BrokerReply
    {
        winrt::Windows::Data::Json::JsonObject result{ nullptr };
        std::wstring activationId;
        double roundTripMilliseconds = 0.0;
        uint64_t responseUtf8Bytes = 0;
    };

    BrokerReply SendBrokerCommand(
        WorkerProcessTopologyTransport& transport,
        winrt::Windows::Data::Json::JsonObject const& request,
        std::chrono::milliseconds timeout);
    void TryReleaseBroker(
        WorkerProcessTopologyTransport& transport,
        std::chrono::milliseconds timeout) noexcept;

    std::wstring PayloadSha256(uint64_t bytes);

    winrt::Windows::Data::Json::JsonObject HeartbeatProjection(
        uint64_t interval,
        uint64_t expected,
        uint64_t observed,
        uint64_t maximumGap,
        bool independentWhileMemoryRetained);
    winrt::Windows::Data::Json::JsonObject MemoryProjection(
        uint64_t appLimit,
        uint64_t appUsage,
        uint64_t processCommit,
        uint64_t processResident,
        uint64_t virtualReserved,
        uint64_t requestedTouched,
        uint64_t touched,
        std::wstring const& digest,
        uint64_t mutationCount,
        double retentionSeconds,
        uint64_t limitEventCount);
    winrt::Windows::Data::Json::JsonObject ComponentProjection(
        std::wstring const& componentId,
        std::wstring const& role,
        std::wstring const& packageIdentity,
        std::wstring const& processIdentity,
        std::wstring const& activationIdentity,
        std::wstring const& lifecycleState,
        winrt::Windows::Data::Json::JsonObject const& memory,
        winrt::Windows::Data::Json::JsonObject const& heartbeat,
        bool cpuSchedulingObserved);

    std::wstring SuccessEnvelope(
        std::wstring const& protocolVersion,
        std::wstring const& correlationId,
        winrt::Windows::Data::Json::JsonObject const& package,
        winrt::Windows::Data::Json::JsonObject const& foreground,
        winrt::Windows::Data::Json::JsonObject const& broker,
        winrt::Windows::Data::Json::JsonObject const& memoryRelation,
        winrt::Windows::Data::Json::JsonObject const& ipc,
        winrt::Windows::Data::Json::JsonObject const& lifecycle,
        winrt::Windows::Data::Json::JsonObject const& storage);
}