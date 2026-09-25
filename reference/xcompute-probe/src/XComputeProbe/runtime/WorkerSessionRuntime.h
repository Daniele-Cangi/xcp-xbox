#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerProtocolBoundary.h"

namespace XComputeProbe
{
    class WorkerSessionRuntime
    {
    public:
        explicit WorkerSessionRuntime(std::wstring protocolVersion);

        static constexpr wchar_t const* SchemaVersion()
        {
            return L"worker-session-runtime-0.1";
        }

        static constexpr uint64_t DefaultTtlSeconds = 15 * 60;
        static constexpr uint64_t MinTtlSeconds = 30;
        static constexpr uint64_t MaxTtlSeconds = 60 * 60;
        static constexpr size_t MaxClosedSessionTombstones = 128;

        void EnsurePairingCode();
        std::wstring PairingCode() const;
        uint64_t ActiveSessionCount() const;

        void RequirePairing(
            winrt::Windows::Data::Json::JsonObject const& request) const;
        void RequireAuthorized(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& command,
            bool refreshSession = true);
        bool ValidateSession(
            std::wstring const& sessionId,
            uint64_t* remainingSeconds = nullptr,
            bool refreshSession = true);

        std::wstring OpenSession(
            winrt::Windows::Data::Json::JsonObject const& request);
        std::wstring CreateSession(
            uint64_t requestedTtl,
            std::wstring const& commandName,
            std::wstring const& extraJson = L"");
        std::wstring CloseSession(
            winrt::Windows::Data::Json::JsonObject const& request);
        std::wstring SessionStatus(
            winrt::Windows::Data::Json::JsonObject const& request);

    private:
        struct SessionState
        {
            std::chrono::steady_clock::time_point expiresAt;
            uint64_t ttlSeconds = 0;
        };

        static std::wstring GeneratePairingCode();
        static std::wstring GenerateSessionId();
        void PruneExpiredSessionsLocked();
        bool IsKnownClosedLocked(std::wstring const& sessionId) const;
        void RememberClosedLocked(std::wstring const& sessionId);

        std::wstring protocolVersion_;
        std::wstring pairingCode_;
        std::map<std::wstring, SessionState> sessions_;
        std::deque<std::wstring> closedSessionIds_;
        mutable std::mutex sessionMutex_;
    };
}
