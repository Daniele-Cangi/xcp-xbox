#include "pch.h"
#include "WorkerProcessTopologyBrokerPhaseRuntime.h"

#include "WorkerProcessTopologyBrokerProtocol.h"

#include <algorithm>
#include <array>
#include <cwctype>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;

namespace winrt::XComputeTopologyBroker::implementation
{
    namespace
    {
        constexpr std::array<std::wstring_view, 11> PhaseOrder{
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

        struct PhaseBinding final
        {
            std::wstring matrixExecutionId;
            std::wstring matrixRequestSha256;
            std::wstring phaseId;
            uint64_t sequence = 0;
            uint64_t attempt = 0;
            bool previousHashPresent = false;
            std::wstring previousHash;
            bool resumeAfterActivation = false;
        };

        bool IsSafeIdentifier(std::wstring const& value)
        {
            return !value.empty() && value.size() <= 96 &&
                std::all_of(value.begin(), value.end(), [](wchar_t ch)
                {
                    return
                        (ch >= L'a' && ch <= L'z') ||
                        (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'0' && ch <= L'9') ||
                        ch == L'.' || ch == L'_' || ch == L'-';
                });
        }

        bool IsLowerHex(std::wstring const& value, size_t size)
        {
            return value.size() == size &&
                std::all_of(value.begin(), value.end(), [](wchar_t ch)
                {
                    return
                        (ch >= L'0' && ch <= L'9') ||
                        (ch >= L'a' && ch <= L'f');
                });
        }

        bool IsRunId(std::wstring const& value)
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

        std::wstring ReadRequiredString(
            JsonObject const& request,
            wchar_t const* name)
        {
            if (!request.HasKey(name) ||
                request.GetNamedValue(name).ValueType() !=
                    JsonValueType::String)
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    std::wstring(L"phase field must be a string: ") + name);
            }
            return std::wstring(
                request.GetNamedString(name).c_str());
        }

        PhaseBinding ReadBinding(
            JsonObject const& request,
            std::wstring_view expectedCommand)
        {
            auto schema = ReadRequiredString(
                request,
                L"phase_control_schema");
            auto command = ReadRequiredString(request, L"command");
            if (schema != BrokerPhaseControlSchema ||
                std::wstring_view(command) != expectedCommand)
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    L"phase control schema or operation is not supported");
            }

            PhaseBinding binding;
            binding.matrixExecutionId = ReadRequiredString(
                request,
                L"matrix_execution_id");
            binding.matrixRequestSha256 = ReadRequiredString(
                request,
                L"matrix_request_sha256");
            binding.phaseId = ReadRequiredString(request, L"phase_id");
            binding.sequence = ReadBrokerIntegral(
                request,
                L"phase_sequence",
                0,
                true);
            binding.attempt = ReadBrokerIntegral(
                request,
                L"phase_attempt",
                0,
                true);
            if (!request.HasKey(L"previous_phase_result_sha256"))
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    L"previous_phase_result_sha256 is required");
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
                binding.previousHash = std::wstring(
                    previous.GetString().c_str());
            }
            else
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    L"previous_phase_result_sha256 must be null or a lowercase SHA-256");
            }
            if (!request.HasKey(L"resume_after_activation"))
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    L"resume_after_activation is required");
            }
            binding.resumeAfterActivation = ReadBrokerBoolean(
                request,
                L"resume_after_activation",
                false);

            if (!IsSafeIdentifier(binding.matrixExecutionId) ||
                !IsLowerHex(binding.matrixRequestSha256, 64) ||
                binding.sequence == 0 ||
                binding.sequence > BrokerPhaseCount ||
                binding.attempt == 0 ||
                binding.attempt > BrokerMaximumPhaseAttempts ||
                std::wstring_view(binding.phaseId) != PhaseOrder[
                    static_cast<size_t>(binding.sequence - 1)] ||
                (binding.previousHashPresent &&
                    !IsLowerHex(binding.previousHash, 64)))
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_BINDING",
                    L"phase binding violates the fixed T2 order, attempt, identifier, or hash bounds");
            }
            return binding;
        }

        std::wstring NewRunId()
        {
            auto value = std::wstring(
                to_hstring(GuidHelper::CreateNewGuid()).c_str());
            value.erase(
                std::remove_if(
                    value.begin(),
                    value.end(),
                    [](wchar_t ch)
                    {
                        return ch == L'{' || ch == L'}';
                    }),
                value.end());
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(std::towlower(ch));
                });
            if (!IsRunId(value))
            {
                FailBrokerRequest(
                    L"INTERNAL_PHASE_STATE",
                    L"the broker did not produce a canonical phase run id");
            }
            return value;
        }

        void PutNull(JsonObject const& object, wchar_t const* name)
        {
            object.SetNamedValue(
                name,
                JsonValue::CreateNullValue());
        }

        JsonArray PhaseOrderJson()
        {
            JsonArray order;
            for (auto phase : PhaseOrder)
            {
                order.Append(
                    JsonValue::CreateStringValue(hstring(phase)));
            }
            return order;
        }
    }

    bool WorkerProcessTopologyBrokerPhaseRuntime::BindingEquals(
        std::wstring const& matrixExecutionId,
        std::wstring const& matrixRequestSha256,
        std::wstring const& phaseId,
        uint64_t sequence,
        uint64_t attempt,
        bool previousHashPresent,
        std::wstring const& previousHash,
        bool resumeAfterActivation) const noexcept
    {
        return
            matrixExecutionId_ == matrixExecutionId &&
            matrixRequestSha256_ == matrixRequestSha256 &&
            phaseId_ == phaseId &&
            phaseSequence_ == sequence &&
            phaseAttempt_ == attempt &&
            previousHashPresent_ == previousHashPresent &&
            (!previousHashPresent ||
                previousPhaseResultSha256_ == previousHash) &&
            resumeAfterActivation_ == resumeAfterActivation;
    }

    JsonObject WorkerProcessTopologyBrokerPhaseRuntime::Begin(
        JsonObject const& request,
        BrokerHeartbeatSnapshot const& heartbeat)
    {
        auto binding = ReadBinding(request, L"phase_begin");
        if (!heartbeat.active)
        {
            FailBrokerRequest(
                L"PHASE_HEARTBEAT_INACTIVE",
                L"phase assignment requires an active autonomous heartbeat");
        }

        auto exact = BindingEquals(
            binding.matrixExecutionId,
            binding.matrixRequestSha256,
            binding.phaseId,
            binding.sequence,
            binding.attempt,
            binding.previousHashPresent,
            binding.previousHash,
            binding.resumeAfterActivation);
        if (state_ != State::Unassigned && exact)
        {
            return TransitionResult(L"BEGIN", true, heartbeat);
        }
        if (state_ == State::Active)
        {
            FailBrokerRequest(
                L"PHASE_ALREADY_ACTIVE",
                L"a different phase cannot begin while the current phase is active");
        }

        if (state_ == State::Unassigned)
        {
            auto first =
                binding.sequence == 1 && binding.attempt == 1;
            if (first &&
                (binding.previousHashPresent ||
                 binding.resumeAfterActivation))
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_TRANSITION",
                    L"the first phase must start without a previous result or activation resume");
            }
            if (!first &&
                (!binding.previousHashPresent ||
                 !binding.resumeAfterActivation))
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_TRANSITION",
                    L"a non-first phase on a fresh broker activation requires a previous result and explicit resume");
            }
        }
        else
        {
            if (binding.matrixExecutionId != matrixExecutionId_ ||
                binding.matrixRequestSha256 != matrixRequestSha256_ ||
                binding.resumeAfterActivation ||
                !binding.previousHashPresent)
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_TRANSITION",
                    L"same-activation phase advancement must preserve matrix binding and carry the previous result");
            }
            auto retry =
                binding.sequence == phaseSequence_ &&
                binding.phaseId == phaseId_ &&
                binding.attempt == phaseAttempt_ + 1;
            auto advance =
                binding.sequence == phaseSequence_ + 1 &&
                binding.attempt == 1;
            if (!retry && !advance)
            {
                FailBrokerRequest(
                    L"INVALID_PHASE_TRANSITION",
                    L"phase advancement must be the next phase or the next bounded attempt of the current phase");
            }
        }

        matrixExecutionId_ = binding.matrixExecutionId;
        matrixRequestSha256_ = binding.matrixRequestSha256;
        phaseId_ = binding.phaseId;
        phaseSequence_ = binding.sequence;
        phaseAttempt_ = binding.attempt;
        previousHashPresent_ = binding.previousHashPresent;
        previousPhaseResultSha256_ = binding.previousHash;
        resumeAfterActivation_ = binding.resumeAfterActivation;
        phaseRunId_ = NewRunId();
        state_ = State::Active;
        ++transitionSequence_;
        startedMonotonicMilliseconds_ =
            heartbeat.observedMonotonicMilliseconds;
        completedMonotonicMilliseconds_ = 0;
        heartbeatSequenceAtStart_ = heartbeat.sequence;
        heartbeatSequenceAtComplete_ = 0;
        return TransitionResult(L"BEGIN", false, heartbeat);
    }

    JsonObject WorkerProcessTopologyBrokerPhaseRuntime::Complete(
        JsonObject const& request,
        BrokerHeartbeatSnapshot const& heartbeat)
    {
        auto binding = ReadBinding(request, L"phase_complete");
        auto runId = ReadRequiredString(request, L"phase_run_id");
        if (!IsRunId(runId) || state_ == State::Unassigned ||
            !BindingEquals(
                binding.matrixExecutionId,
                binding.matrixRequestSha256,
                binding.phaseId,
                binding.sequence,
                binding.attempt,
                binding.previousHashPresent,
                binding.previousHash,
                binding.resumeAfterActivation) ||
            runId != phaseRunId_)
        {
            FailBrokerRequest(
                L"PHASE_BINDING_MISMATCH",
                L"phase completion does not match the active broker phase run");
        }
        if (state_ == State::Completed)
        {
            return TransitionResult(L"COMPLETE", true, heartbeat);
        }
        if (!heartbeat.active ||
            heartbeat.sequence <= heartbeatSequenceAtStart_)
        {
            FailBrokerRequest(
                L"PHASE_HEARTBEAT_NOT_OBSERVED",
                L"phase completion requires an autonomous heartbeat sample after phase begin");
        }

        state_ = State::Completed;
        ++transitionSequence_;
        completedMonotonicMilliseconds_ =
            heartbeat.observedMonotonicMilliseconds;
        heartbeatSequenceAtComplete_ = heartbeat.sequence;
        return TransitionResult(L"COMPLETE", false, heartbeat);
    }

    JsonObject WorkerProcessTopologyBrokerPhaseRuntime::TransitionResult(
        std::wstring_view operation,
        bool idempotent,
        BrokerHeartbeatSnapshot const& heartbeat) const
    {
        JsonObject result;
        PutBrokerString(
            result,
            L"schema_version",
            BrokerPhaseControlResultSchema);
        PutBrokerString(result, L"operation", operation);
        PutBrokerBoolean(
            result,
            L"idempotent_success",
            idempotent);
        result.SetNamedValue(
            L"phase_status",
            Status(heartbeat));
        return result;
    }

    bool WorkerProcessTopologyBrokerPhaseRuntime::AllowsDiagnosticStorage(
        std::wstring_view phaseRunId) const noexcept
    {
        return state_ == State::Active &&
            phaseId_ == L"storage_visibility" &&
            phaseRunId_ == phaseRunId;
    }

    JsonObject WorkerProcessTopologyBrokerPhaseRuntime::Status(
        BrokerHeartbeatSnapshot const& heartbeat) const
    {
        JsonObject phase;
        if (state_ == State::Unassigned)
        {
            PutBrokerString(phase, L"state", L"UNASSIGNED");
            PutNull(phase, L"matrix_execution_id");
            PutNull(phase, L"matrix_request_sha256");
            PutNull(phase, L"phase_id");
            PutBrokerNumber(phase, L"sequence", 0);
            PutBrokerNumber(phase, L"attempt", 0);
            PutNull(phase, L"previous_phase_result_sha256");
            PutBrokerBoolean(phase, L"resume_after_activation", false);
            PutNull(phase, L"phase_run_id");
            PutBrokerNumber(phase, L"transition_sequence", 0);
            PutNull(phase, L"started_monotonic_ms");
            PutNull(phase, L"completed_monotonic_ms");
            PutNull(phase, L"heartbeat_sequence_at_start");
            PutNull(phase, L"heartbeat_sequence_at_complete");
            PutBrokerNumber(
                phase,
                L"heartbeat_samples_observed",
                0);
        }
        else
        {
            PutBrokerString(
                phase,
                L"state",
                state_ == State::Active ? L"ACTIVE" : L"COMPLETED");
            PutBrokerString(
                phase,
                L"matrix_execution_id",
                matrixExecutionId_);
            PutBrokerString(
                phase,
                L"matrix_request_sha256",
                matrixRequestSha256_);
            PutBrokerString(phase, L"phase_id", phaseId_);
            PutBrokerNumber(phase, L"sequence", phaseSequence_);
            PutBrokerNumber(phase, L"attempt", phaseAttempt_);
            if (previousHashPresent_)
            {
                PutBrokerString(
                    phase,
                    L"previous_phase_result_sha256",
                    previousPhaseResultSha256_);
            }
            else
            {
                PutNull(phase, L"previous_phase_result_sha256");
            }
            PutBrokerBoolean(
                phase,
                L"resume_after_activation",
                resumeAfterActivation_);
            PutBrokerString(phase, L"phase_run_id", phaseRunId_);
            PutBrokerNumber(
                phase,
                L"transition_sequence",
                transitionSequence_);
            PutBrokerNumber(
                phase,
                L"started_monotonic_ms",
                startedMonotonicMilliseconds_);
            if (state_ == State::Completed)
            {
                PutBrokerNumber(
                    phase,
                    L"completed_monotonic_ms",
                    completedMonotonicMilliseconds_);
            }
            else
            {
                PutNull(phase, L"completed_monotonic_ms");
            }
            PutBrokerNumber(
                phase,
                L"heartbeat_sequence_at_start",
                heartbeatSequenceAtStart_);
            if (state_ == State::Completed)
            {
                PutBrokerNumber(
                    phase,
                    L"heartbeat_sequence_at_complete",
                    heartbeatSequenceAtComplete_);
            }
            else
            {
                PutNull(phase, L"heartbeat_sequence_at_complete");
            }
            auto end = state_ == State::Completed
                ? heartbeatSequenceAtComplete_
                : heartbeat.sequence;
            PutBrokerNumber(
                phase,
                L"heartbeat_samples_observed",
                end >= heartbeatSequenceAtStart_
                    ? end - heartbeatSequenceAtStart_
                    : 0);
        }

        JsonObject authority;
        PutBrokerBoolean(authority, L"semantic", false);
        PutBrokerBoolean(authority, L"evidence_acceptance", false);
        PutBrokerBoolean(authority, L"classification", false);
        PutBrokerBoolean(authority, L"artifact_publication", false);

        JsonObject result;
        PutBrokerString(
            result,
            L"schema_version",
            BrokerPhaseStatusSchema);
        PutBrokerBoolean(result, L"read_only", true);
        PutBrokerBoolean(
            result,
            L"phase_assignment_supported",
            true);
        PutBrokerBoolean(
            result,
            L"full_t2_phase_instrumentation_complete",
            true);
        PutBrokerString(
            result,
            L"phase_control_schema",
            BrokerPhaseControlSchema);
        result.SetNamedValue(L"phase_order", PhaseOrderJson());
        result.SetNamedValue(L"authority", authority);
        result.SetNamedValue(L"phase", phase);
        result.SetNamedValue(
            L"autonomous_heartbeat",
            heartbeat.ToJson());
        return result;
    }
}