#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe::ProcessTopology
{
    inline constexpr wchar_t const* BrokerPhaseStatusSchemaV1 =
        L"xcp-process-topology-broker-phase-status-v1";
    inline constexpr wchar_t const* BrokerPhaseStatusSchema =
        L"xcp-process-topology-broker-phase-status-v2";
    inline constexpr wchar_t const* BrokerPhaseControlSchema =
        L"xcp-process-topology-broker-phase-control-v1";
    inline constexpr wchar_t const* BrokerPhaseControlResultSchema =
        L"xcp-process-topology-broker-phase-control-result-v1";
    inline constexpr wchar_t const* PhaseRequestSchemaV1 =
        L"xcp-process-topology-t2-phase-v1-runtime-request";
    inline constexpr wchar_t const* PhaseRequestSchema =
        L"xcp-process-topology-t2-phase-v2-runtime-request";

    struct BrokerPhaseBinding
    {
        std::wstring matrixExecutionId;
        std::wstring matrixRequestSha256;
        std::wstring phaseId;
        uint64_t sequence = 0;
        uint64_t attempt = 0;
        bool previousHashPresent = false;
        std::wstring previousPhaseResultSha256;
        bool resumeAfterActivation = false;
    };

    struct BrokerPhaseStatus
    {
        winrt::Windows::Data::Json::JsonObject raw{ nullptr };
        winrt::Windows::Data::Json::JsonObject autonomousHeartbeat{ nullptr };
        std::wstring phaseState;
        std::wstring matrixExecutionId;
        std::wstring matrixRequestSha256;
        std::wstring phaseId;
        uint64_t phaseSequence = 0;
        uint64_t phaseAttempt = 0;
        bool previousHashPresent = false;
        std::wstring previousPhaseResultSha256;
        bool resumeAfterActivation = false;
        std::wstring phaseRunId;
        uint64_t transitionSequence = 0;
        uint64_t phaseStartedMonotonicMilliseconds = 0;
        uint64_t phaseCompletedMonotonicMilliseconds = 0;
        uint64_t heartbeatSequenceAtPhaseStart = 0;
        uint64_t heartbeatSequenceAtPhaseComplete = 0;
        uint64_t heartbeatSamplesObserved = 0;
        uint64_t intervalMilliseconds = 0;
        uint64_t sequence = 0;
        uint64_t startedMonotonicMilliseconds = 0;
        uint64_t lastTickMonotonicMilliseconds = 0;
        uint64_t observedMonotonicMilliseconds = 0;
        uint64_t maximumGapMilliseconds = 0;
    };

    struct BrokerPhaseTransition
    {
        BrokerPhaseStatus status;
        std::wstring operation;
        bool idempotentSuccess = false;
    };

    BrokerPhaseBinding ParseBrokerPhaseBindingRequest(
        winrt::Windows::Data::Json::JsonObject const& request);
    winrt::Windows::Data::Json::JsonObject NewBrokerPhaseBeginCommand(
        BrokerPhaseBinding const& binding);
    winrt::Windows::Data::Json::JsonObject NewBrokerPhaseCompleteCommand(
        BrokerPhaseBinding const& binding,
        std::wstring const& phaseRunId);
    BrokerPhaseStatus ParseBrokerPhaseStatus(
        winrt::Windows::Data::Json::JsonObject const& result);
    BrokerPhaseTransition ParseBrokerPhaseTransition(
        winrt::Windows::Data::Json::JsonObject const& result,
        std::wstring_view expectedOperation);
    winrt::Windows::Data::Json::JsonObject
        NewBrokerAutonomousHeartbeatProjection(
            BrokerPhaseStatus const& before,
            BrokerPhaseStatus const& after);
    bool BrokerPhaseMatches(
        BrokerPhaseStatus const& status,
        BrokerPhaseBinding const& binding,
        std::wstring_view expectedState) noexcept;
}