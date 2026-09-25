#include "pch.h"
#include "WorkerSessionRuntime.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
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

        uint64_t GetOptionalUInt64(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t fallback = 0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }

            auto value = object.GetNamedNumber(name);
            if (value < 0)
            {
                throw WorkerProtocolError(
                    "argument.invalid_number",
                    "numeric argument cannot be negative");
            }
            return static_cast<uint64_t>(value);
        }

        wchar_t const* BoolJson(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring OkBase(
            std::wstring const& protocolVersion,
            std::wstring const& command)
        {
            return L"{\"ok\":true,\"protocol_version\":" +
                JsonString(protocolVersion) +
                L",\"command\":" +
                JsonString(command);
        }
    }

    WorkerSessionRuntime::WorkerSessionRuntime(std::wstring protocolVersion) :
        protocolVersion_(std::move(protocolVersion))
    {
    }

    std::wstring WorkerSessionRuntime::GeneratePairingCode()
    {
        auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        uint32_t value = static_cast<uint32_t>(ticks ^ (ticks >> 32));
        try
        {
            std::random_device random;
            value ^= random();
        }
        catch (...)
        {
        }

        std::wostringstream out;
        out << std::setw(6) << std::setfill(L'0') << (value % 1000000);
        return out.str();
    }

    std::wstring WorkerSessionRuntime::GenerateSessionId()
    {
        auto ticks = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        uint64_t first = ticks;
        uint64_t second = ticks ^ 0xa5a5a5a55a5a5a5aull;
        try
        {
            std::random_device random;
            first ^= (static_cast<uint64_t>(random()) << 32) ^ random();
            second ^= (static_cast<uint64_t>(random()) << 32) ^ random();
        }
        catch (...)
        {
        }

        std::wostringstream out;
        out << std::hex << std::setfill(L'0')
            << std::setw(16) << first
            << std::setw(16) << second;
        return out.str();
    }

    void WorkerSessionRuntime::EnsurePairingCode()
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (pairingCode_.empty())
        {
            pairingCode_ = GeneratePairingCode();
        }
    }

    std::wstring WorkerSessionRuntime::PairingCode() const
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        return pairingCode_;
    }

    uint64_t WorkerSessionRuntime::ActiveSessionCount() const
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(std::count_if(
            sessions_.begin(),
            sessions_.end(),
            [&](auto const& entry)
            {
                return entry.second.expiresAt > now;
            }));
    }

    void WorkerSessionRuntime::RequirePairing(JsonObject const& request) const
    {
        auto supplied = GetOptionalString(request, L"pairing_code");
        if (supplied.empty())
        {
            throw WorkerProtocolError(
                "auth.required",
                "pairing_code is required");
        }

        std::wstring expected;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            expected = pairingCode_;
        }
        if (supplied != expected)
        {
            throw WorkerProtocolError(
                "auth.invalid",
                "pairing_code is invalid");
        }
    }

    void WorkerSessionRuntime::RememberClosedLocked(
        std::wstring const& sessionId)
    {
        if (sessionId.empty() || IsKnownClosedLocked(sessionId))
        {
            return;
        }
        closedSessionIds_.push_back(sessionId);
        while (closedSessionIds_.size() > MaxClosedSessionTombstones)
        {
            closedSessionIds_.pop_front();
        }
    }

    bool WorkerSessionRuntime::IsKnownClosedLocked(
        std::wstring const& sessionId) const
    {
        return std::find(
            closedSessionIds_.begin(),
            closedSessionIds_.end(),
            sessionId) != closedSessionIds_.end();
    }

    void WorkerSessionRuntime::PruneExpiredSessionsLocked()
    {
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();)
        {
            if (it->second.expiresAt <= now)
            {
                RememberClosedLocked(it->first);
                it = sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool WorkerSessionRuntime::ValidateSession(
        std::wstring const& sessionId,
        uint64_t* remainingSeconds,
        bool refreshSession)
    {
        if (sessionId.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(sessionMutex_);
        PruneExpiredSessionsLocked();
        auto found = sessions_.find(sessionId);
        if (found == sessions_.end())
        {
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            found->second.expiresAt - now);
        if (refreshSession)
        {
            found->second.expiresAt =
                now + std::chrono::seconds(found->second.ttlSeconds);
            remaining = std::chrono::seconds(found->second.ttlSeconds);
        }
        if (remainingSeconds != nullptr)
        {
            *remainingSeconds = static_cast<uint64_t>(
                (std::max)(int64_t{ 0 }, remaining.count()));
        }
        return true;
    }

    void WorkerSessionRuntime::RequireAuthorized(
        JsonObject const& request,
        std::wstring const& command,
        bool refreshSession)
    {
        auto sessionId = GetOptionalString(request, L"session_id");
        if (!sessionId.empty())
        {
            if (ValidateSession(sessionId, nullptr, refreshSession))
            {
                return;
            }
            throw WorkerProtocolError(
                "auth.session_invalid",
                "session_id is invalid or expired");
        }

        auto suppliedPairing = GetOptionalString(request, L"pairing_code");
        if (!suppliedPairing.empty())
        {
            RequirePairing(request);
            return;
        }

        if (command == L"open_session")
        {
            throw WorkerProtocolError(
                "auth.required",
                "pairing_code is required to open a session");
        }
        throw WorkerProtocolError(
            "auth.required",
            "pairing_code or session_id is required");
    }

    std::wstring WorkerSessionRuntime::CreateSession(
        uint64_t requestedTtl,
        std::wstring const& commandName,
        std::wstring const& extraJson)
    {
        auto ttl = (std::min)(
            MaxTtlSeconds,
            (std::max)(MinTtlSeconds, requestedTtl));

        std::lock_guard<std::mutex> lock(sessionMutex_);
        PruneExpiredSessionsLocked();

        std::wstring sessionId;
        do
        {
            sessionId = GenerateSessionId();
        } while (sessions_.find(sessionId) != sessions_.end());

        sessions_[sessionId] = SessionState{
            std::chrono::steady_clock::now() + std::chrono::seconds(ttl),
            ttl
        };

        return OkBase(protocolVersion_, commandName) +
            L",\"schema_version\":" + JsonString(SchemaVersion()) +
            L",\"session_id\":" + JsonString(sessionId) +
            L",\"ttl_seconds\":" + std::to_wstring(ttl) +
            L",\"expires_in_seconds\":" + std::to_wstring(ttl) +
            L",\"active_session_count\":" + std::to_wstring(sessions_.size()) +
            extraJson +
            L"}";
    }

    std::wstring WorkerSessionRuntime::OpenSession(JsonObject const& request)
    {
        RequirePairing(request);
        auto requestedTtl = GetOptionalUInt64(
            request,
            L"ttl_seconds",
            DefaultTtlSeconds);
        return CreateSession(requestedTtl, L"open_session");
    }

    std::wstring WorkerSessionRuntime::CloseSession(JsonObject const& request)
    {
        auto sessionId = GetOptionalString(request, L"session_id");
        if (sessionId.empty())
        {
            throw WorkerProtocolError(
                "auth.session_required",
                "session_id is required to close a session");
        }

        auto suppliedPairing = GetOptionalString(request, L"pairing_code");
        auto authorizedByPairing = !suppliedPairing.empty();
        if (authorizedByPairing)
        {
            RequirePairing(request);
        }

        bool closed = false;
        size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            PruneExpiredSessionsLocked();
            auto found = sessions_.find(sessionId);
            auto knownClosed = IsKnownClosedLocked(sessionId);
            if (!authorizedByPairing &&
                found == sessions_.end() &&
                !knownClosed)
            {
                throw WorkerProtocolError(
                    "auth.session_invalid",
                    "session_id is invalid or expired");
            }

            if (found != sessions_.end())
            {
                sessions_.erase(found);
                RememberClosedLocked(sessionId);
                closed = true;
            }
            remaining = sessions_.size();
        }

        return OkBase(protocolVersion_, L"close_session") +
            L",\"schema_version\":" + JsonString(SchemaVersion()) +
            L",\"session_id\":" + JsonString(sessionId) +
            L",\"closed\":" + BoolJson(closed) +
            L",\"already_closed_or_expired\":" + BoolJson(!closed) +
            L",\"idempotent\":true" +
            L",\"active_session_count\":" + std::to_wstring(remaining) +
            L"}";
    }

    std::wstring WorkerSessionRuntime::SessionStatus(JsonObject const& request)
    {
        auto sessionId = GetOptionalString(request, L"session_id");
        if (sessionId.empty())
        {
            throw WorkerProtocolError(
                "auth.session_required",
                "session_id is required for session_status");
        }

        uint64_t remainingSeconds = 0;
        auto active = ValidateSession(sessionId, &remainingSeconds);
        if (!active && !GetOptionalString(request, L"pairing_code").empty())
        {
            RequirePairing(request);
        }
        else if (!active)
        {
            throw WorkerProtocolError(
                "auth.session_invalid",
                "session_id is invalid or expired");
        }

        size_t activeCount = 0;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            PruneExpiredSessionsLocked();
            activeCount = sessions_.size();
        }

        return OkBase(protocolVersion_, L"session_status") +
            L",\"schema_version\":" + JsonString(SchemaVersion()) +
            L",\"session_id\":" + JsonString(sessionId) +
            L",\"active\":" + BoolJson(active) +
            L",\"expires_in_seconds\":" +
            std::to_wstring(remainingSeconds) +
            L",\"refresh_applied\":" + BoolJson(active) +
            L",\"active_session_count\":" +
            std::to_wstring(activeCount) +
            L"}";
    }
}
