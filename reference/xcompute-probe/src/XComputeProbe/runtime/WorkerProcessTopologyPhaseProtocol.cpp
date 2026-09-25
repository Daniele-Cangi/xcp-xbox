#include "pch.h"
#include "WorkerProcessTopologyProtocol.h"

#include <array>
#include <utility>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {        constexpr std::array<std::wstring_view, 11>
            FullT2PhaseOrder{
                L"idle_baseline",
                L"foreground_cpu",
                L"foreground_gpu_verified",
                L"foreground_cpu_gpu_verified",
                L"progressive_memory_pressure",
                L"storage_visibility",
                L"foreground_suspend_resume",
                L"foreground_terminate_restart",
                L"auxiliary_terminate_restart",
                L"package_redeploy",
                L"final_cleanup_quiet" };

        bool IsCanonicalRunId(std::wstring const& value)
        {
            if (value.size() != 36)
            {
                return false;
            }
            for (size_t index = 0; index != value.size(); ++index)
            {
                auto hyphen =
                    index == 8 || index == 13 ||
                    index == 18 || index == 23;
                auto hex =
                    (value[index] >= L'0' && value[index] <= L'9') ||
                    (value[index] >= L'a' && value[index] <= L'f');
                if ((hyphen && value[index] != L'-') ||
                    (!hyphen && !hex))
                {
                    return false;
                }
            }
            return true;
        }
    }

    BrokerPhaseBinding ParseBrokerPhaseBindingRequest(
        JsonObject const& request)
    {
        constexpr wchar_t const* errorCode =
            L"topology.request_schema_invalid";
        auto schema = RequiredString(
            request,
            L"phase_control_schema",
            errorCode);
        BrokerPhaseBinding binding;
        binding.matrixExecutionId = RequiredString(
            request, L"matrix_execution_id", errorCode);
        binding.matrixRequestSha256 = RequiredString(
            request, L"matrix_request_sha256", errorCode);
        binding.phaseId = RequiredString(
            request, L"phase_id", errorCode);
        binding.sequence = RequiredUInt64(
            request, L"phase_sequence", errorCode);
        binding.attempt = RequiredUInt64(
            request, L"phase_attempt", errorCode);
        binding.resumeAfterActivation = RequiredBool(
            request, L"resume_after_activation", errorCode);
        if (!request.HasKey(L"previous_phase_result_sha256"))
        {
            Fail(errorCode, L"previous_phase_result_sha256 is required");
        }
        auto previous = request.GetNamedValue(
            L"previous_phase_result_sha256");
        if (previous.ValueType() == JsonValueType::Null)
        {
            binding.previousHashPresent = false;
        }
        else if (previous.ValueType() == JsonValueType::String)
        {
            binding.previousHashPresent = true;
            binding.previousPhaseResultSha256 =
                std::wstring(previous.GetString().c_str());
        }
        else
        {
            Fail(
                errorCode,
                L"previous_phase_result_sha256 must be null or a lowercase SHA-256");
        }

        if (schema != BrokerPhaseControlSchema ||
            !IsSafeToken(binding.matrixExecutionId, 96) ||
            !IsLowerHex(binding.matrixRequestSha256, 64) ||
            binding.sequence == 0 ||
            binding.sequence > FullT2PhaseOrder.size() ||
            binding.attempt == 0 ||
            binding.attempt > 8 ||
            std::wstring_view(binding.phaseId) !=
                FullT2PhaseOrder[
                    static_cast<size_t>(binding.sequence - 1)] ||
            (binding.previousHashPresent &&
                !IsLowerHex(
                    binding.previousPhaseResultSha256,
                    64)))
        {
            Fail(
                errorCode,
                L"the T2 phase binding violates schema, order, attempt, identifier, or hash bounds");
        }
        auto first =
            binding.sequence == 1 && binding.attempt == 1;
        if (first &&
            (binding.previousHashPresent ||
             binding.resumeAfterActivation))
        {
            Fail(
                errorCode,
                L"the first T2 phase cannot resume or bind a previous result");
        }
        if (!first &&
            (!binding.previousHashPresent ||
             !binding.resumeAfterActivation))
        {
            Fail(
                errorCode,
                L"a non-first phase request must explicitly resume a previous result on the new broker activation");
        }
        return binding;
    }

    JsonObject NewBrokerPhaseBeginCommand(
        BrokerPhaseBinding const& binding)
    {
        auto request = NewBrokerCommand(L"phase_begin");
        PutString(
            request,
            L"phase_control_schema",
            BrokerPhaseControlSchema);
        PutString(
            request,
            L"matrix_execution_id",
            binding.matrixExecutionId);
        PutString(
            request,
            L"matrix_request_sha256",
            binding.matrixRequestSha256);
        PutString(request, L"phase_id", binding.phaseId);
        PutNumber(request, L"phase_sequence", binding.sequence);
        PutNumber(request, L"phase_attempt", binding.attempt);
        if (binding.previousHashPresent)
        {
            PutString(
                request,
                L"previous_phase_result_sha256",
                binding.previousPhaseResultSha256);
        }
        else
        {
            PutNull(request, L"previous_phase_result_sha256");
        }
        PutBool(
            request,
            L"resume_after_activation",
            binding.resumeAfterActivation);
        return request;
    }

    JsonObject NewBrokerPhaseCompleteCommand(
        BrokerPhaseBinding const& binding,
        std::wstring const& phaseRunId)
    {
        auto request = NewBrokerPhaseBeginCommand(binding);
        PutString(request, L"command", L"phase_complete");
        PutString(request, L"phase_run_id", phaseRunId);
        return request;
    }

    BrokerPhaseStatus ParseBrokerPhaseStatus(
        JsonObject const& result)
    {
        constexpr wchar_t const* errorCode =
            L"topology.app_service_payload_invalid";
        auto requireNull = [&](JsonObject const& object, wchar_t const* name)
        {
            if (!object || !object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Null)
            {
                Fail(errorCode, L"a required null field is invalid", name);
            }
        };
        auto nullableString = [&](
            JsonObject const& object,
            wchar_t const* name)
        {
            std::pair<bool, std::wstring> value;
            if (!object.HasKey(name))
            {
                Fail(errorCode, L"a nullable string field is absent", name);
            }
            auto item = object.GetNamedValue(name);
            if (item.ValueType() == JsonValueType::Null)
            {
                return value;
            }
            if (item.ValueType() != JsonValueType::String)
            {
                Fail(errorCode, L"a nullable string field has the wrong type", name);
            }
            value.first = true;
            value.second = std::wstring(item.GetString().c_str());
            return value;
        };
        auto nullableUInt = [&](
            JsonObject const& object,
            wchar_t const* name)
        {
            std::pair<bool, uint64_t> value;
            if (!object.HasKey(name))
            {
                Fail(errorCode, L"a nullable integer field is absent", name);
            }
            auto item = object.GetNamedValue(name);
            if (item.ValueType() == JsonValueType::Null)
            {
                return value;
            }
            value.first = true;
            value.second = RequiredUInt64(object, name, errorCode);
            return value;
        };

        if (!result || result.Size() != 9 ||
            RequiredString(
                result, L"schema_version", errorCode) !=
                BrokerPhaseStatusSchema ||
            !RequiredBool(result, L"read_only", errorCode) ||
            !RequiredBool(
                result, L"phase_assignment_supported", errorCode) ||
            !RequiredBool(
                result,
                L"full_t2_phase_instrumentation_complete",
                errorCode) ||
            RequiredString(
                result, L"phase_control_schema", errorCode) !=
                BrokerPhaseControlSchema)
        {
            Fail(errorCode, L"the broker phase-status v2 identity or boundary is invalid");
        }

        if (!result.HasKey(L"phase_order") ||
            result.GetNamedValue(L"phase_order").ValueType() !=
                JsonValueType::Array)
        {
            Fail(errorCode, L"the broker phase order is absent or invalid");
        }
        auto phaseOrder = result.GetNamedArray(L"phase_order");
        if (phaseOrder.Size() != FullT2PhaseOrder.size())
        {
            Fail(errorCode, L"the broker phase order has the wrong cardinality");
        }
        for (uint32_t index = 0; index != phaseOrder.Size(); ++index)
        {
            if (phaseOrder.GetStringAt(index) !=
                FullT2PhaseOrder[index])
            {
                Fail(errorCode, L"the broker phase order differs from the fixed T2 tuple");
            }
        }

        auto authority = RequiredObject(
            result, L"authority", errorCode);
        if (authority.Size() != 4 ||
            RequiredBool(authority, L"semantic", errorCode) ||
            RequiredBool(
                authority, L"evidence_acceptance", errorCode) ||
            RequiredBool(
                authority, L"classification", errorCode) ||
            RequiredBool(
                authority, L"artifact_publication", errorCode))
        {
            Fail(errorCode, L"the broker phase-status authority boundary changed");
        }

        auto phase = RequiredObject(result, L"phase", errorCode);
        if (phase.Size() != 15)
        {
            Fail(errorCode, L"the broker phase object has the wrong cardinality");
        }
        BrokerPhaseStatus status;
        status.raw = result;
        status.phaseState = RequiredString(
            phase, L"state", errorCode);
        status.phaseSequence = RequiredUInt64(
            phase, L"sequence", errorCode);
        status.phaseAttempt = RequiredUInt64(
            phase, L"attempt", errorCode);
        status.resumeAfterActivation = RequiredBool(
            phase, L"resume_after_activation", errorCode);
        status.transitionSequence = RequiredUInt64(
            phase, L"transition_sequence", errorCode);
        status.heartbeatSamplesObserved = RequiredUInt64(
            phase, L"heartbeat_samples_observed", errorCode);
        auto matrixId = nullableString(
            phase, L"matrix_execution_id");
        auto requestHash = nullableString(
            phase, L"matrix_request_sha256");
        auto phaseId = nullableString(phase, L"phase_id");
        auto previousHash = nullableString(
            phase, L"previous_phase_result_sha256");
        auto runId = nullableString(phase, L"phase_run_id");
        auto phaseStarted = nullableUInt(
            phase, L"started_monotonic_ms");
        auto phaseCompleted = nullableUInt(
            phase, L"completed_monotonic_ms");
        auto heartbeatAtStart = nullableUInt(
            phase, L"heartbeat_sequence_at_start");
        auto heartbeatAtComplete = nullableUInt(
            phase, L"heartbeat_sequence_at_complete");

        if (status.phaseState == L"UNASSIGNED")
        {
            if (matrixId.first || requestHash.first || phaseId.first ||
                previousHash.first || runId.first || phaseStarted.first ||
                phaseCompleted.first || heartbeatAtStart.first ||
                heartbeatAtComplete.first ||
                status.phaseSequence != 0 || status.phaseAttempt != 0 ||
                status.resumeAfterActivation ||
                status.transitionSequence != 0 ||
                status.heartbeatSamplesObserved != 0)
            {
                Fail(errorCode, L"the unassigned broker phase contains mutable state");
            }
        }
        else if (status.phaseState == L"ACTIVE" ||
                 status.phaseState == L"COMPLETED")
        {
            status.matrixExecutionId = matrixId.second;
            status.matrixRequestSha256 = requestHash.second;
            status.phaseId = phaseId.second;
            status.previousHashPresent = previousHash.first;
            status.previousPhaseResultSha256 = previousHash.second;
            status.phaseRunId = runId.second;
            status.phaseStartedMonotonicMilliseconds =
                phaseStarted.second;
            status.phaseCompletedMonotonicMilliseconds =
                phaseCompleted.second;
            status.heartbeatSequenceAtPhaseStart =
                heartbeatAtStart.second;
            status.heartbeatSequenceAtPhaseComplete =
                heartbeatAtComplete.second;
            if (!matrixId.first || !requestHash.first ||
                !phaseId.first || !runId.first || !phaseStarted.first ||
                !heartbeatAtStart.first ||
                !IsSafeToken(status.matrixExecutionId, 96) ||
                !IsLowerHex(status.matrixRequestSha256, 64) ||
                !IsCanonicalRunId(status.phaseRunId) ||
                status.phaseSequence == 0 ||
                status.phaseSequence > FullT2PhaseOrder.size() ||
                status.phaseAttempt == 0 ||
                status.phaseAttempt > 8 ||
                std::wstring_view(status.phaseId) !=
                    FullT2PhaseOrder[
                        static_cast<size_t>(
                            status.phaseSequence - 1)] ||
                (previousHash.first &&
                    !IsLowerHex(previousHash.second, 64)) ||
                status.transitionSequence == 0)
            {
                Fail(errorCode, L"the assigned broker phase binding is invalid");
            }
            if (status.phaseState == L"ACTIVE" &&
                (phaseCompleted.first || heartbeatAtComplete.first))
            {
                Fail(errorCode, L"an active phase contains completion state");
            }
            if (status.phaseState == L"COMPLETED" &&
                (!phaseCompleted.first || !heartbeatAtComplete.first ||
                 phaseCompleted.second < phaseStarted.second ||
                 heartbeatAtComplete.second <= heartbeatAtStart.second ||
                 status.heartbeatSamplesObserved !=
                    heartbeatAtComplete.second - heartbeatAtStart.second ||
                 status.heartbeatSamplesObserved == 0))
            {
                Fail(errorCode, L"the completed phase lacks monotonic heartbeat evidence");
            }
        }
        else
        {
            Fail(errorCode, L"the broker phase state is not supported");
        }

        auto heartbeat = RequiredObject(
            result, L"autonomous_heartbeat", errorCode);
        status.autonomousHeartbeat = heartbeat;
        auto source = RequiredString(
            heartbeat, L"source", errorCode);
        auto active = RequiredBool(
            heartbeat, L"active", errorCode);
        status.intervalMilliseconds = RequiredUInt64(
            heartbeat, L"interval_ms", errorCode);
        status.sequence = RequiredUInt64(
            heartbeat, L"sequence", errorCode);
        status.startedMonotonicMilliseconds = RequiredUInt64(
            heartbeat, L"started_monotonic_ms", errorCode);
        status.lastTickMonotonicMilliseconds = RequiredUInt64(
            heartbeat, L"last_tick_monotonic_ms", errorCode);
        status.observedMonotonicMilliseconds = RequiredUInt64(
            heartbeat, L"observed_monotonic_ms", errorCode);
        status.maximumGapMilliseconds = RequiredUInt64(
            heartbeat, L"maximum_gap_ms", errorCode);
        if (heartbeat.Size() != 8 ||
            source != L"BROKER_PERIODIC_THREAD_POOL_TIMER" ||
            !active ||
            status.intervalMilliseconds !=
                BrokerAutonomousHeartbeatIntervalMilliseconds ||
            status.startedMonotonicMilliseconds == 0 ||
            status.lastTickMonotonicMilliseconds <
                status.startedMonotonicMilliseconds ||
            status.observedMonotonicMilliseconds <
                status.lastTickMonotonicMilliseconds ||
            (status.phaseState != L"UNASSIGNED" &&
                status.heartbeatSequenceAtPhaseStart >
                    status.sequence))
        {
            Fail(errorCode, L"the broker autonomous heartbeat is invalid or non-monotonic");
        }
        return status;
    }

    BrokerPhaseTransition ParseBrokerPhaseTransition(
        JsonObject const& result,
        std::wstring_view expectedOperation)
    {
        constexpr wchar_t const* errorCode =
            L"topology.app_service_payload_invalid";
        auto schema = RequiredString(
            result, L"schema_version", errorCode);
        auto operation = RequiredString(
            result, L"operation", errorCode);
        auto idempotent = RequiredBool(
            result, L"idempotent_success", errorCode);
        if (result.Size() != 4 ||
            schema != BrokerPhaseControlResultSchema ||
            std::wstring_view(operation) != expectedOperation)
        {
            Fail(errorCode, L"the broker phase transition result is invalid");
        }
        return BrokerPhaseTransition{
            ParseBrokerPhaseStatus(
                RequiredObject(
                    result,
                    L"phase_status",
                    errorCode)),
            operation,
            idempotent };
    }

    JsonObject NewBrokerAutonomousHeartbeatProjection(
        BrokerPhaseStatus const& before,
        BrokerPhaseStatus const& after)
    {
        constexpr wchar_t const* errorCode =
            L"topology.app_service_payload_invalid";
        if (before.intervalMilliseconds == 0 ||
            after.intervalMilliseconds != before.intervalMilliseconds ||
            after.startedMonotonicMilliseconds !=
                before.startedMonotonicMilliseconds ||
            after.observedMonotonicMilliseconds <
                before.observedMonotonicMilliseconds ||
            after.sequence <= before.sequence ||
            after.lastTickMonotonicMilliseconds <=
                before.lastTickMonotonicMilliseconds)
        {
            Fail(
                errorCode,
                L"the broker heartbeat projection window is non-monotonic");
        }
        auto windowMilliseconds =
            after.observedMonotonicMilliseconds -
            before.observedMonotonicMilliseconds;
        auto elapsedIntervals =
            windowMilliseconds / after.intervalMilliseconds;
        auto expectedSamples =
            elapsedIntervals > 1 ? elapsedIntervals - 1 : 1;
        return HeartbeatProjection(
            after.intervalMilliseconds,
            expectedSamples,
            after.sequence - before.sequence,
            after.maximumGapMilliseconds,
            true);
    }

    bool BrokerPhaseMatches(
        BrokerPhaseStatus const& status,
        BrokerPhaseBinding const& binding,
        std::wstring_view expectedState) noexcept
    {
        return
            std::wstring_view(status.phaseState) == expectedState &&
            status.matrixExecutionId == binding.matrixExecutionId &&
            status.matrixRequestSha256 ==
                binding.matrixRequestSha256 &&
            status.phaseId == binding.phaseId &&
            status.phaseSequence == binding.sequence &&
            status.phaseAttempt == binding.attempt &&
            status.previousHashPresent ==
                binding.previousHashPresent &&
            (!binding.previousHashPresent ||
                status.previousPhaseResultSha256 ==
                    binding.previousPhaseResultSha256) &&
            status.resumeAfterActivation ==
                binding.resumeAfterActivation &&
            !status.phaseRunId.empty();
    }
}
