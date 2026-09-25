#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <winrt/Windows.Data.Json.h>

#include "WorkerProcessTopologyBrokerHeartbeat.h"

namespace winrt::XComputeTopologyBroker::implementation
{
    inline constexpr wchar_t const* BrokerPhaseStatusSchema =
        L"xcp-process-topology-broker-phase-status-v2";
    inline constexpr wchar_t const* BrokerPhaseControlSchema =
        L"xcp-process-topology-broker-phase-control-v1";
    inline constexpr wchar_t const* BrokerPhaseControlResultSchema =
        L"xcp-process-topology-broker-phase-control-result-v1";
    inline constexpr uint64_t BrokerPhaseCount = 11;
    inline constexpr uint64_t BrokerMaximumPhaseAttempts = 8;

    class WorkerProcessTopologyBrokerPhaseRuntime final
    {
    public:
        Windows::Data::Json::JsonObject Begin(
            Windows::Data::Json::JsonObject const& request,
            BrokerHeartbeatSnapshot const& heartbeat);
        Windows::Data::Json::JsonObject Complete(
            Windows::Data::Json::JsonObject const& request,
            BrokerHeartbeatSnapshot const& heartbeat);
        Windows::Data::Json::JsonObject Status(
            BrokerHeartbeatSnapshot const& heartbeat) const;
        bool AllowsDiagnosticStorage(
            std::wstring_view phaseRunId) const noexcept;

    private:
        enum class State
        {
            Unassigned,
            Active,
            Completed,
        };

        Windows::Data::Json::JsonObject TransitionResult(
            std::wstring_view operation,
            bool idempotent,
            BrokerHeartbeatSnapshot const& heartbeat) const;
        bool BindingEquals(
            std::wstring const& matrixExecutionId,
            std::wstring const& matrixRequestSha256,
            std::wstring const& phaseId,
            uint64_t sequence,
            uint64_t attempt,
            bool previousHashPresent,
            std::wstring const& previousHash,
            bool resumeAfterActivation) const noexcept;

        State state_ = State::Unassigned;
        std::wstring matrixExecutionId_;
        std::wstring matrixRequestSha256_;
        std::wstring phaseId_;
        uint64_t phaseSequence_ = 0;
        uint64_t phaseAttempt_ = 0;
        bool previousHashPresent_ = false;
        std::wstring previousPhaseResultSha256_;
        bool resumeAfterActivation_ = false;
        std::wstring phaseRunId_;
        uint64_t transitionSequence_ = 0;
        uint64_t startedMonotonicMilliseconds_ = 0;
        uint64_t completedMonotonicMilliseconds_ = 0;
        uint64_t heartbeatSequenceAtStart_ = 0;
        uint64_t heartbeatSequenceAtComplete_ = 0;
    };
}