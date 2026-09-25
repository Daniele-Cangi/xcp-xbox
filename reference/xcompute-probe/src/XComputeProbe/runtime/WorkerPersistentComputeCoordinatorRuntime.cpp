#include "pch.h"
#include "WorkerPersistentComputeCoordinatorRuntime.h"

#include "WorkerArtifactStore.h"
#include "WorkerContentAddressedStoreRecovery.h"
#include "WorkerProcessTopologyProtocol.h"
#include "WorkerProcessTopologyTransport.h"
#include "WorkerProvableWorldsStreamingRuntime.h"
#include "WorkerStorageScaleCharacterizationRuntime.h"
#include "../../shared/XcpStorageReservationLedger.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe
{
    namespace
    {
        using namespace ProcessTopology;

        constexpr wchar_t const* CoordinatorRequestSchema =
            L"xcp-persistent-compute-coordinator-reservation-v1-runtime-request";
        constexpr wchar_t const* CoordinatorResultSchema =
            L"xcp-persistent-compute-coordinator-reservation-v1-runtime-result";
        constexpr wchar_t const* DescriptionSchema =
            L"xcp-persistent-compute-coordinator-description-v1";
        constexpr wchar_t const* StoreRecoveryRequestSchema =
            L"xcp-content-addressed-store-recovery-v1-runtime-request";
        constexpr wchar_t const* StoreRecoveryResultSchema =
            L"xcp-content-addressed-store-recovery-v1-runtime-result";

        std::wstring Sha256(std::wstring const& text)
        {
            auto utf8 = to_string(hstring(text));
            std::vector<uint8_t> bytes(
                utf8.begin(),
                utf8.end());
            auto provider = HashAlgorithmProvider::OpenAlgorithm(
                HashAlgorithmNames::Sha256());
            auto value = std::wstring(
                CryptographicBuffer::EncodeToHexString(
                    provider.HashData(
                        CryptographicBuffer::CreateFromByteArray(
                            bytes))).c_str());
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(
                        std::towlower(ch));
                });
            return value;
        }

        bool RetentionMixAllowed(std::wstring const& value)
        {
            return
                value == L"EPHEMERAL" ||
                value == L"EPHEMERAL_CACHE" ||
                value == L"EPHEMERAL_RECOVERY" ||
                value == L"EPHEMERAL_RECOVERY_PINNED";
        }

        JsonArray RequiredArray(
            JsonObject const& object,
            wchar_t const* name)
        {
            if (!object ||
                !object.HasKey(name) ||
                object.GetNamedValue(name).ValueType() !=
                    JsonValueType::Array)
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"a required broker array is absent or invalid",
                    name);
            }
            return object.GetNamedArray(name);
        }

        void RequireTransitionSequence(
            JsonObject const& reservation,
            std::vector<std::wstring> const& expected)
        {
            auto transitions = RequiredArray(
                reservation,
                L"transitions");
            if (transitions.Size() != expected.size())
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the broker reservation transition count is invalid");
            }
            for (uint32_t index = 0;
                 index != transitions.Size();
                 ++index)
            {
                auto value = transitions.GetStringAt(index);
                if (std::wstring(value.c_str()) != expected[index])
                {
                    Fail(
                        L"persistent_compute.broker_response_invalid",
                        L"the broker reservation transition order is invalid");
                }
            }
        }

        JsonObject AdmissionJson(
            XComputeShared::XcpStorageAdmissionDecision const& decision)
        {
            JsonObject value;
            PutBool(value, L"side_effect_free", true);
            PutBool(value, L"saturating_arithmetic", true);
            PutBool(value, L"admitted", decision.admitted);
            PutString(value, L"code", decision.code);
            PutNumber(
                value,
                L"protected_existing_bytes",
                decision.protectedExistingBytes);
            PutNumber(
                value,
                L"active_reservation_bytes",
                decision.activeReservationBytes);
            PutNumber(
                value,
                L"required_against_configured_quota_bytes",
                decision.requiredAgainstConfiguredQuotaBytes);
            PutNumber(
                value,
                L"configured_usable_bytes",
                decision.configuredUsableBytes);
            PutNumber(
                value,
                L"required_against_platform_available_bytes",
                decision.requiredAgainstPlatformAvailableBytes);
            return value;
        }

        JsonObject PolicyJson(
            XComputeShared::XcpStorageQuotaPolicy const& policy)
        {
            JsonObject value;
            PutString(
                value,
                L"policy_version",
                XComputeShared::XcpCoordinatorPolicyVersion);
            PutString(
                value,
                L"profile_status",
                L"CONFIGURED_BOUNDED_CANDIDATE_NOT_PLATFORM_CAPACITY");
            PutNumber(
                value,
                L"global_configured_bytes",
                policy.globalConfiguredBytes);
            PutNumber(
                value,
                L"safety_reserve_bytes",
                policy.safetyReserveBytes);
            PutNumber(
                value,
                L"per_job_bytes",
                policy.perJobBytes);
            PutNumber(
                value,
                L"metadata_journal_allowance_bytes",
                policy.metadataJournalAllowanceBytes);
            PutNumber(
                value,
                L"logical_page_bytes",
                policy.logicalPageBytes);
            PutNumber(
                value,
                L"maximum_active_reservations",
                policy.maximumActiveReservations);
            PutNumber(
                value,
                L"maximum_retained_records",
                policy.maximumRetainedRecords);
            PutNumber(
                value,
                L"maximum_simultaneous_buffers",
                policy.maximumSimultaneousBuffers);
            PutString(
                value,
                L"numeric_watermarks_status",
                L"UNKNOWN_PENDING_MEASUREMENT");
            return value;
        }

        void AttachBinding(
            JsonObject const& request,
            XComputeShared::XcpReservationBinding const& binding)
        {
            PutString(
                request,
                L"reservation_schema_version",
                XComputeShared::XcpReservationLeaseSchema);
            PutString(request, L"job_id", binding.jobId);
            PutString(
                request,
                L"execution_id",
                binding.executionId);
            PutString(
                request,
                L"storage_plan_sha256",
                binding.storagePlanSha256);
            PutNumber(
                request,
                L"declared_peak_bytes",
                binding.declaredPeakBytes);
            PutNumber(
                request,
                L"generation",
                binding.generation);
            PutString(
                request,
                L"owner_identity",
                binding.ownerIdentity);
            auto const& domains = binding.quotaDomains;
            PutNumber(
                request,
                L"protected_recovery_pinned_bytes",
                domains.protectedRecoveryPinnedBytes);
            PutNumber(
                request,
                L"evictable_cache_ephemeral_bytes",
                domains.evictableCacheEphemeralBytes);
            PutNumber(
                request,
                L"persistent_artifact_bytes",
                domains.persistentArtifactBytes);
            PutNumber(
                request,
                L"metadata_journal_bytes",
                domains.metadataJournalBytes);
            PutNumber(
                request,
                L"temporary_bytes",
                domains.temporaryBytes);
            PutNumber(
                request,
                L"declared_physical_growth_bytes",
                domains.declaredPhysicalGrowthBytes);
            PutBool(
                request,
                L"platform_available_known",
                domains.platformAvailableKnown);
            PutNumber(
                request,
                L"platform_available_bytes",
                domains.platformAvailableBytes);
            PutNumber(
                request,
                L"maximum_simultaneous_buffers",
                domains.maximumSimultaneousBuffers);
        }

        JsonObject ErrorResult(
            std::wstring const& protocolVersion,
            std::wstring const& correlationId,
            std::wstring const& code,
            std::wstring const& message,
            std::wstring const& detail)
        {
            JsonObject value;
            PutString(
                value,
                L"schema_version",
                CoordinatorResultSchema);
            PutBool(value, L"ok", false);
            PutString(
                value,
                L"delivery_train",
                XComputeShared::XcpCoordinatorTrainId);
            PutString(
                value,
                L"protocol_version",
                protocolVersion);
            PutString(
                value,
                L"correlation_id",
                correlationId);
            JsonObject error;
            PutString(error, L"code", code);
            PutString(error, L"message", message);
            PutString(error, L"detail", detail);
            value.SetNamedValue(L"error", error);
            return value;
        }

        void ValidateDescription(
            JsonObject const& description,
            XComputeShared::XcpStorageQuotaPolicy const& policy)
        {
            if (RequiredString(
                    description,
                    L"schema_version",
                    L"persistent_compute.broker_response_invalid") !=
                    DescriptionSchema ||
                RequiredString(
                    description,
                    L"delivery_train",
                    L"persistent_compute.broker_response_invalid") !=
                    XComputeShared::XcpCoordinatorTrainId ||
                RequiredString(
                    description,
                    L"selected_topology",
                    L"persistent_compute.broker_response_invalid") !=
                    L"CASE_B_OUT_OF_PROCESS_NARROW_COORDINATOR" ||
                RequiredBool(
                    description,
                    L"semantic_authority",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    description,
                    L"artifact_publication_authority",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    description,
                    L"page_store_materialized",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    description,
                    L"persistent_journal_materialized",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    description,
                    L"second_job_queue",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    description,
                    L"second_artifact_system",
                    L"persistent_compute.broker_response_invalid") ||
                !RequiredBool(
                    description,
                    L"single_process_adapter_available",
                    L"persistent_compute.broker_response_invalid"))
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the broker coordinator role or authority boundary is invalid");
            }
            auto remotePolicy = RequiredObject(
                description,
                L"policy",
                L"persistent_compute.broker_response_invalid");
            if (RequiredString(
                    remotePolicy,
                    L"policy_version",
                    L"persistent_compute.broker_response_invalid") !=
                    XComputeShared::XcpCoordinatorPolicyVersion ||
                RequiredUInt64(
                    remotePolicy,
                    L"global_configured_bytes",
                    L"persistent_compute.broker_response_invalid") !=
                    policy.globalConfiguredBytes ||
                RequiredUInt64(
                    remotePolicy,
                    L"safety_reserve_bytes",
                    L"persistent_compute.broker_response_invalid") !=
                    policy.safetyReserveBytes ||
                RequiredUInt64(
                    remotePolicy,
                    L"per_job_bytes",
                    L"persistent_compute.broker_response_invalid") !=
                    policy.perJobBytes)
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the worker and broker reservation policies disagree");
            }
        }

        JsonObject ValidateReservationOperation(
            BrokerReply const& reply,
            XComputeShared::XcpReservationBinding const& binding,
            std::wstring const& expectedCode,
            bool expectedIdempotent,
            std::wstring const& expectedState,
            std::vector<std::wstring> const& expectedTransitions)
        {
            auto const& result = reply.result;
            if (RequiredString(
                    result,
                    L"schema_version",
                    L"persistent_compute.broker_response_invalid") !=
                    XComputeShared::XcpReservationLeaseSchema ||
                !RequiredBool(
                    result,
                    L"succeeded",
                    L"persistent_compute.broker_response_invalid") ||
                RequiredBool(
                    result,
                    L"idempotent",
                    L"persistent_compute.broker_response_invalid") !=
                    expectedIdempotent ||
                RequiredString(
                    result,
                    L"code",
                    L"persistent_compute.broker_response_invalid") !=
                    expectedCode)
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the broker reservation operation envelope is invalid");
            }
            auto reservation = RequiredObject(
                result,
                L"reservation",
                L"persistent_compute.broker_response_invalid");
            if (RequiredString(
                    reservation,
                    L"job_id",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.jobId ||
                RequiredString(
                    reservation,
                    L"execution_id",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.executionId ||
                RequiredString(
                    reservation,
                    L"storage_plan_sha256",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.storagePlanSha256 ||
                RequiredUInt64(
                    reservation,
                    L"declared_peak_bytes",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.declaredPeakBytes ||
                RequiredUInt64(
                    reservation,
                    L"generation",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.generation ||
                RequiredString(
                    reservation,
                    L"owner_identity",
                    L"persistent_compute.broker_response_invalid") !=
                    binding.ownerIdentity ||
                RequiredString(
                    reservation,
                    L"state",
                    L"persistent_compute.broker_response_invalid") !=
                    expectedState)
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the broker reservation binding does not match the admitted plan");
            }
            auto reservationId = RequiredString(
                reservation,
                L"reservation_id",
                L"persistent_compute.broker_response_invalid");
            if (!IsSafeToken(reservationId, 96))
            {
                Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"the broker reservation identity is invalid");
            }
            RequireTransitionSequence(
                reservation,
                expectedTransitions);
            return reservation;
        }

        JsonObject Subgate(
            std::wstring_view gate,
            std::wstring_view status)
        {
            JsonObject value;
            PutString(value, L"gate", gate);
            PutString(value, L"status", status);
            return value;
        }
    }

    std::wstring WorkerPersistentComputeCoordinatorRuntime::Probe(
        JsonObject const& request,
        std::wstring const& protocolVersion)
    {
        std::wstring correlationId = L"persistent-compute";
        try
        {
            auto schema = ProcessTopology::RequiredString(
                request,
                L"schema_version",
                L"persistent_compute.request_schema_invalid");
            if (schema != CoordinatorRequestSchema)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.request_schema_invalid",
                    L"the persistent compute request schema is unsupported",
                    schema);
            }
            auto command = ProcessTopology::RequiredString(
                request,
                L"command",
                L"persistent_compute.request_schema_invalid");
            if (command !=
                L"probe_persistent_compute_coordinator")
            {
                ProcessTopology::Fail(
                    L"persistent_compute.request_schema_invalid",
                    L"the persistent compute command is invalid",
                    command);
            }
            correlationId = ProcessTopology::RequiredString(
                request,
                L"correlation_id",
                L"persistent_compute.request_schema_invalid");
            auto scenario = ProcessTopology::RequiredString(
                request,
                L"scenario",
                L"persistent_compute.request_schema_invalid");
            auto jobId = ProcessTopology::RequiredString(
                request,
                L"job_id",
                L"persistent_compute.request_schema_invalid");
            auto executionId = ProcessTopology::RequiredString(
                request,
                L"execution_id",
                L"persistent_compute.request_schema_invalid");
            auto executionPlanSha256 =
                ProcessTopology::RequiredString(
                    request,
                    L"execution_plan_sha256",
                    L"persistent_compute.request_schema_invalid");
            auto ownerIdentity = ProcessTopology::RequiredString(
                request,
                L"owner_identity",
                L"persistent_compute.request_schema_invalid");
            auto retentionMix = ProcessTopology::RequiredString(
                request,
                L"retention_mix",
                L"persistent_compute.request_schema_invalid");
            if (!ProcessTopology::IsSafeToken(
                    correlationId,
                    64) ||
                !ProcessTopology::IsSafeToken(jobId, 64) ||
                !ProcessTopology::IsSafeToken(executionId, 64) ||
                !ProcessTopology::IsSafeToken(ownerIdentity, 96) ||
                !ProcessTopology::IsLowerHex(
                    executionPlanSha256,
                    64) ||
                !RetentionMixAllowed(retentionMix))
            {
                ProcessTopology::Fail(
                    L"persistent_compute.request_schema_invalid",
                    L"one or more bounded identity, digest, or retention fields are invalid");
            }
            if (scenario != L"positive" &&
                scenario != L"negative_zero_peak" &&
                scenario != L"negative_over_job_cap" &&
                scenario != L"negative_platform_space")
            {
                ProcessTopology::Fail(
                    L"persistent_compute.request_schema_invalid",
                    L"scenario is not allowlisted",
                    scenario);
            }

            XComputeShared::XcpStorageQuotaDomains domains;
            domains.protectedRecoveryPinnedBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"protected_recovery_pinned_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.evictableCacheEphemeralBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"evictable_cache_ephemeral_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.persistentArtifactBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"persistent_artifact_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.metadataJournalBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"metadata_journal_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.temporaryBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"temporary_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.declaredPeakBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"declared_peak_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.declaredPhysicalGrowthBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"declared_physical_growth_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.platformAvailableKnown =
                ProcessTopology::RequiredBool(
                    request,
                    L"platform_available_known",
                    L"persistent_compute.request_schema_invalid");
            domains.platformAvailableBytes =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"platform_available_bytes",
                    L"persistent_compute.request_schema_invalid");
            domains.maximumSimultaneousBuffers =
                ProcessTopology::RequiredUInt64(
                    request,
                    L"maximum_simultaneous_buffers",
                    L"persistent_compute.request_schema_invalid");
            auto generation = ProcessTopology::RequiredUInt64(
                request,
                L"generation",
                L"persistent_compute.request_schema_invalid");
            if (generation == 0)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.request_schema_invalid",
                    L"generation must be nonzero before static admission");
            }

            auto policy =
                XComputeShared::XcpDefaultStorageQuotaPolicy();
            auto canonicalPlan =
                std::wstring(
                    XComputeShared::XcpCoordinatorPolicyVersion) +
                L"|logical_page_bytes=" +
                std::to_wstring(policy.logicalPageBytes) +
                L"|declared_peak_bytes=" +
                std::to_wstring(domains.declaredPeakBytes) +
                L"|retention_mix=" + retentionMix +
                L"|maximum_simultaneous_buffers=" +
                std::to_wstring(
                    domains.maximumSimultaneousBuffers) +
                L"|execution_plan_sha256=" +
                executionPlanSha256;
            auto storagePlanSha256 = Sha256(canonicalPlan);

            auto admission =
                XComputeShared::XcpEvaluateStorageAdmission(
                    policy,
                    domains,
                    0);
            std::wstring expectedNegativeCode;
            if (scenario == L"negative_zero_peak")
            {
                expectedNegativeCode = L"DECLARED_PEAK_REQUIRED";
            }
            else if (scenario == L"negative_over_job_cap")
            {
                expectedNegativeCode = L"PER_JOB_QUOTA_EXCEEDED";
            }
            else if (scenario == L"negative_platform_space")
            {
                expectedNegativeCode =
                    L"PLATFORM_AVAILABLE_MINUS_RESERVE_EXCEEDED";
            }

            JsonObject result;
            PutString(
                result,
                L"schema_version",
                CoordinatorResultSchema);
            PutBool(result, L"ok", true);
            PutString(
                result,
                L"delivery_train",
                XComputeShared::XcpCoordinatorTrainId);
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            PutString(result, L"scenario", scenario);
            PutString(
                result,
                L"storage_plan_sha256",
                storagePlanSha256);
            PutString(
                result,
                L"execution_plan_sha256",
                executionPlanSha256);
            PutString(
                result,
                L"retention_mix",
                retentionMix);
            result.SetNamedValue(
                L"policy",
                PolicyJson(policy));
            result.SetNamedValue(
                L"static_admission",
                AdmissionJson(admission));

            if (!expectedNegativeCode.empty())
            {
                if (admission.admitted ||
                    admission.code != expectedNegativeCode)
                {
                    ProcessTopology::Fail(
                        L"persistent_compute.negative_not_refused",
                        L"the expected side-effect-free admission refusal did not occur",
                        admission.code);
                }
                PutString(
                    result,
                    L"outcome",
                    L"EXPECTED_STATIC_REFUSAL");
                PutBool(
                    result,
                    L"broker_activation_attempted",
                    false);
                JsonObject subgates;
                subgates.SetNamedValue(
                    L"t4_component_selection_lifecycle",
                    Subgate(
                        L"T4_MINIMAL_BROKER_OR_LOCAL_STORAGE_SERVICE",
                        L"NOT_APPLICABLE_STATIC_NEGATIVE"));
                subgates.SetNamedValue(
                    L"t5_quota_reservation_admission",
                    Subgate(
                        L"T5_BOUNDED_STORAGE_POLICY_AND_RESERVATION",
                        L"PASS_EXPECTED_FAIL_CLOSED"));
                result.SetNamedValue(L"subgates", subgates);
                return std::wstring(result.Stringify().c_str());
            }
            if (!admission.admitted)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.static_admission_refused",
                    L"the positive storage plan failed static admission",
                    admission.code);
            }

            XComputeShared::XcpReservationBinding binding;
            binding.jobId = jobId;
            binding.executionId = executionId;
            binding.storagePlanSha256 = storagePlanSha256;
            binding.declaredPeakBytes =
                domains.declaredPeakBytes;
            binding.generation = generation;
            binding.ownerIdentity = ownerIdentity;
            binding.quotaDomains = domains;

            ProcessTopology::MonotonicDeadline deadline;
            ProcessTopology::WorkerProcessTopologyTransport
                transport;
            auto family = Package::Current().Id().FamilyName();
            auto opened = transport.Open(
                ProcessTopology::AppServiceName,
                std::wstring_view(family),
                deadline.Remaining());
            if (!opened.succeeded)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.broker_open_failed",
                    L"the selected Case B coordinator did not activate",
                    std::to_wstring(opened.statusCode));
            }

            auto descriptionReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    ProcessTopology::NewBrokerCommand(
                        L"coordinator_describe"),
                    deadline.Remaining());
            ValidateDescription(
                descriptionReply.result,
                policy);

            auto acquireRequest =
                ProcessTopology::NewBrokerCommand(
                    L"reservation_acquire");
            AttachBinding(acquireRequest, binding);
            auto acquireReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    acquireRequest,
                    deadline.Remaining());
            auto acquired = ValidateReservationOperation(
                acquireReply,
                binding,
                L"RESERVATION_ACQUIRED",
                false,
                L"active",
                { L"requested", L"active" });
            auto reservationId = ProcessTopology::RequiredString(
                acquired,
                L"reservation_id",
                L"persistent_compute.broker_response_invalid");

            auto statusRequest =
                ProcessTopology::NewBrokerCommand(
                    L"reservation_status");
            PutString(
                statusRequest,
                L"reservation_id",
                reservationId);
            auto statusReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    statusRequest,
                    deadline.Remaining());
            ValidateReservationOperation(
                statusReply,
                binding,
                L"RESERVATION_FOUND",
                false,
                L"active",
                { L"requested", L"active" });

            auto duplicateReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    acquireRequest,
                    deadline.Remaining());
            auto duplicate = ValidateReservationOperation(
                duplicateReply,
                binding,
                L"RESERVATION_ALREADY_ACTIVE",
                true,
                L"active",
                { L"requested", L"active" });
            if (ProcessTopology::RequiredString(
                    duplicate,
                    L"reservation_id",
                    L"persistent_compute.broker_response_invalid") !=
                reservationId)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"idempotent acquisition changed reservation identity");
            }

            auto releaseRequest =
                ProcessTopology::NewBrokerCommand(
                    L"reservation_release");
            AttachBinding(releaseRequest, binding);
            PutString(
                releaseRequest,
                L"reservation_id",
                reservationId);
            PutString(
                releaseRequest,
                L"terminal_disposition",
                L"probe_completed");
            auto releaseReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    releaseRequest,
                    deadline.Remaining());
            ValidateReservationOperation(
                releaseReply,
                binding,
                L"RESERVATION_RELEASED",
                false,
                L"released",
                {
                    L"requested",
                    L"active",
                    L"releasing",
                    L"released"
                });

            auto releaseReplayReply =
                ProcessTopology::SendBrokerCommand(
                    transport,
                    releaseRequest,
                    deadline.Remaining());
            ValidateReservationOperation(
                releaseReplayReply,
                binding,
                L"RESERVATION_ALREADY_RELEASED",
                true,
                L"released",
                {
                    L"requested",
                    L"active",
                    L"releasing",
                    L"released"
                });
            auto activationStable =
                descriptionReply.activationId ==
                    acquireReply.activationId &&
                acquireReply.activationId ==
                    statusReply.activationId &&
                statusReply.activationId ==
                    duplicateReply.activationId &&
                duplicateReply.activationId ==
                    releaseReply.activationId &&
                releaseReply.activationId ==
                    releaseReplayReply.activationId;
            if (!activationStable)
            {
                ProcessTopology::Fail(
                    L"persistent_compute.broker_response_invalid",
                    L"broker activation identity changed during a lease lifecycle");
            }
            auto closeCompleted = transport.Close();

            PutString(
                result,
                L"outcome",
                L"COORDINATOR_AND_RESERVATION_VERIFIED");
            PutBool(
                result,
                L"broker_activation_attempted",
                true);
            JsonObject topology;
            PutString(
                topology,
                L"selected_class",
                L"B");
            PutString(
                topology,
                L"adapter",
                L"same_package_out_of_process_app_service");
            PutString(
                topology,
                L"component_role",
                L"watchdog_lifecycle_and_reservation_coordinator");
            PutString(
                topology,
                L"activation_id",
                acquireReply.activationId);
            PutBool(
                topology,
                L"activation_identity_stable",
                activationStable);
            PutBool(
                topology,
                L"client_close_completed",
                closeCompleted);
            PutBool(
                topology,
                L"single_process_adapter_available",
                true);
            PutBool(
                topology,
                L"page_store_materialized",
                false);
            PutBool(
                topology,
                L"persistent_journal_materialized",
                false);
            result.SetNamedValue(L"topology", topology);

            JsonObject lifecycle;
            PutString(
                lifecycle,
                L"reservation_id",
                reservationId);
            PutBool(
                lifecycle,
                L"acquire_idempotent",
                true);
            PutBool(
                lifecycle,
                L"release_idempotent",
                true);
            PutBool(
                lifecycle,
                L"worker_broker_binding_agreed",
                true);
            PutString(
                lifecycle,
                L"terminal_disposition",
                L"probe_completed");
            JsonArray transitions;
            for (auto const* state : {
                L"requested",
                L"active",
                L"releasing",
                L"released" })
            {
                transitions.Append(
                    JsonValue::CreateStringValue(state));
            }
            lifecycle.SetNamedValue(
                L"transitions",
                transitions);
            result.SetNamedValue(
                L"reservation_lifecycle",
                lifecycle);

            JsonObject authority;
            PutBool(authority, L"worker_protocol", true);
            PutBool(authority, L"worker_admission", true);
            PutBool(
                authority,
                L"worker_execution_plan",
                true);
            PutBool(
                authority,
                L"worker_final_publication_veto",
                true);
            PutBool(
                authority,
                L"broker_semantic",
                false);
            PutBool(
                authority,
                L"broker_artifact_publication",
                false);
            result.SetNamedValue(
                L"authority",
                authority);

            JsonObject subgates;
            subgates.SetNamedValue(
                L"t4_component_selection_lifecycle",
                Subgate(
                    L"T4_MINIMAL_BROKER_OR_LOCAL_STORAGE_SERVICE",
                    L"PASS"));
            subgates.SetNamedValue(
                L"t5_quota_reservation_admission",
                Subgate(
                    L"T5_BOUNDED_STORAGE_POLICY_AND_RESERVATION",
                    L"PASS"));
            result.SetNamedValue(L"subgates", subgates);
            return std::wstring(result.Stringify().c_str());
        }
        catch (ProcessTopology::Error const& error)
        {
            return std::wstring(
                ErrorResult(
                    protocolVersion,
                    correlationId,
                    error.code,
                    error.message,
                    error.detail).Stringify().c_str());
        }
        catch (hresult_error const& error)
        {
            return std::wstring(
                ErrorResult(
                    protocolVersion,
                    correlationId,
                    L"persistent_compute.platform_failure",
                    L"a packaged platform API failed",
                    std::to_wstring(
                        error.code().value)).Stringify().c_str());
        }
        catch (...)
        {
            return std::wstring(
                ErrorResult(
                    protocolVersion,
                    correlationId,
                    L"persistent_compute.unexpected_failure",
                    L"an unexpected bounded coordinator failure occurred",
                    L"unhandled").Stringify().c_str());
        }
    }

    std::wstring
    WorkerPersistentComputeCoordinatorRuntime::
        ProbeContentAddressedStoreRecovery(
            JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot)
    {
        std::wstring correlationId =
            L"content-addressed-store-recovery";
        try
        {
            auto schema = RequiredString(
                request,
                L"schema_version",
                L"content_store.request_schema_invalid");
            auto command = RequiredString(
                request,
                L"command",
                L"content_store.request_schema_invalid");
            if (schema != StoreRecoveryRequestSchema ||
                command !=
                    L"probe_content_addressed_store_recovery")
            {
                Fail(
                    L"content_store.request_schema_invalid",
                    L"the content-addressed store request contract is invalid");
            }
            correlationId = RequiredString(
                request,
                L"correlation_id",
                L"content_store.request_schema_invalid");
            auto operation = RequiredString(
                request,
                L"operation",
                L"content_store.request_schema_invalid");
            auto namespaceId = RequiredString(
                request,
                L"namespace_id",
                L"content_store.request_schema_invalid");
            auto jobId = RequiredString(
                request,
                L"job_id",
                L"content_store.request_schema_invalid");
            auto executionId = RequiredString(
                request,
                L"execution_id",
                L"content_store.request_schema_invalid");
            auto datasetId = RequiredString(
                request,
                L"dataset_id",
                L"content_store.request_schema_invalid");
            auto executionPlanSha256 = RequiredString(
                request,
                L"execution_plan_sha256",
                L"content_store.request_schema_invalid");
            auto ownerIdentity = RequiredString(
                request,
                L"owner_identity",
                L"content_store.request_schema_invalid");
            auto retentionMix = RequiredString(
                request,
                L"retention_mix",
                L"content_store.request_schema_invalid");
            if (!IsSafeToken(correlationId, 64) ||
                !IsSafeToken(namespaceId, 40) ||
                !IsSafeToken(jobId, 64) ||
                !IsSafeToken(executionId, 64) ||
                !IsSafeToken(datasetId, 64) ||
                !IsSafeToken(ownerIdentity, 96) ||
                !IsLowerHex(executionPlanSha256, 64) ||
                !RetentionMixAllowed(retentionMix) ||
                (operation != L"core_matrix" &&
                    operation != L"seed_restart" &&
                    operation != L"recover_restart" &&
                    operation != L"cleanup_restart"))
            {
                Fail(
                    L"content_store.request_schema_invalid",
                    L"one or more content-store request fields are invalid");
            }

            XComputeShared::XcpStorageQuotaDomains domains;
            domains.protectedRecoveryPinnedBytes =
                RequiredUInt64(
                    request,
                    L"protected_recovery_pinned_bytes",
                    L"content_store.request_schema_invalid");
            domains.evictableCacheEphemeralBytes =
                RequiredUInt64(
                    request,
                    L"evictable_cache_ephemeral_bytes",
                    L"content_store.request_schema_invalid");
            domains.persistentArtifactBytes =
                RequiredUInt64(
                    request,
                    L"persistent_artifact_bytes",
                    L"content_store.request_schema_invalid");
            domains.metadataJournalBytes =
                RequiredUInt64(
                    request,
                    L"metadata_journal_bytes",
                    L"content_store.request_schema_invalid");
            domains.temporaryBytes =
                RequiredUInt64(
                    request,
                    L"temporary_bytes",
                    L"content_store.request_schema_invalid");
            domains.declaredPeakBytes =
                RequiredUInt64(
                    request,
                    L"declared_peak_bytes",
                    L"content_store.request_schema_invalid");
            domains.declaredPhysicalGrowthBytes =
                RequiredUInt64(
                    request,
                    L"declared_physical_growth_bytes",
                    L"content_store.request_schema_invalid");
            domains.platformAvailableKnown =
                RequiredBool(
                    request,
                    L"platform_available_known",
                    L"content_store.request_schema_invalid");
            domains.platformAvailableBytes =
                RequiredUInt64(
                    request,
                    L"platform_available_bytes",
                    L"content_store.request_schema_invalid");
            domains.maximumSimultaneousBuffers =
                RequiredUInt64(
                    request,
                    L"maximum_simultaneous_buffers",
                    L"content_store.request_schema_invalid");
            auto generation = RequiredUInt64(
                request,
                L"generation",
                L"content_store.request_schema_invalid");
            if (generation == 0)
            {
                Fail(
                    L"content_store.request_schema_invalid",
                    L"generation must be nonzero");
            }

            auto policy =
                XComputeShared::XcpDefaultStorageQuotaPolicy();
            auto canonicalPlan =
                std::wstring(
                    XComputeShared::XcpCoordinatorPolicyVersion) +
                L"|logical_page_bytes=" +
                std::to_wstring(policy.logicalPageBytes) +
                L"|declared_peak_bytes=" +
                std::to_wstring(domains.declaredPeakBytes) +
                L"|retention_mix=" + retentionMix +
                L"|maximum_simultaneous_buffers=" +
                std::to_wstring(
                    domains.maximumSimultaneousBuffers) +
                L"|execution_plan_sha256=" +
                executionPlanSha256;
            auto storagePlanSha256 =
                Sha256(canonicalPlan);
            auto admission =
                XComputeShared::XcpEvaluateStorageAdmission(
                    policy,
                    domains,
                    0);
            if (!admission.admitted)
            {
                Fail(
                    L"content_store.static_admission_refused",
                    L"the content-store plan failed static admission",
                    admission.code);
            }

            XComputeShared::XcpReservationBinding leaseBinding;
            leaseBinding.jobId = jobId;
            leaseBinding.executionId = executionId;
            leaseBinding.storagePlanSha256 =
                storagePlanSha256;
            leaseBinding.declaredPeakBytes =
                domains.declaredPeakBytes;
            leaseBinding.generation = generation;
            leaseBinding.ownerIdentity = ownerIdentity;
            leaseBinding.quotaDomains = domains;

            MonotonicDeadline deadline;
            WorkerProcessTopologyTransport transport;
            auto family =
                Package::Current().Id().FamilyName();
            auto opened = transport.Open(
                AppServiceName,
                std::wstring_view(family),
                deadline.Remaining());
            if (!opened.succeeded)
            {
                Fail(
                    L"content_store.broker_open_failed",
                    L"the Case B reservation coordinator did not activate",
                    std::to_wstring(opened.statusCode));
            }
            auto descriptionReply =
                SendBrokerCommand(
                    transport,
                    NewBrokerCommand(
                        L"coordinator_describe"),
                    deadline.Remaining());
            ValidateDescription(
                descriptionReply.result,
                policy);

            auto acquireRequest =
                NewBrokerCommand(
                    L"reservation_acquire");
            AttachBinding(
                acquireRequest,
                leaseBinding);
            auto acquireReply =
                SendBrokerCommand(
                    transport,
                    acquireRequest,
                    deadline.Remaining());
            auto acquired =
                ValidateReservationOperation(
                    acquireReply,
                    leaseBinding,
                    L"RESERVATION_ACQUIRED",
                    false,
                    L"active",
                    { L"requested", L"active" });
            auto reservationId = RequiredString(
                acquired,
                L"reservation_id",
                L"content_store.broker_response_invalid");

            auto statusRequest =
                NewBrokerCommand(
                    L"reservation_status");
            PutString(
                statusRequest,
                L"reservation_id",
                reservationId);
            auto statusReply =
                SendBrokerCommand(
                    transport,
                    statusRequest,
                    deadline.Remaining());
            ValidateReservationOperation(
                statusReply,
                leaseBinding,
                L"RESERVATION_FOUND",
                false,
                L"active",
                { L"requested", L"active" });

            auto releaseLease = [&]()
            {
                auto releaseRequest =
                    NewBrokerCommand(
                        L"reservation_release");
                AttachBinding(
                    releaseRequest,
                    leaseBinding);
                PutString(
                    releaseRequest,
                    L"reservation_id",
                    reservationId);
                PutString(
                    releaseRequest,
                    L"terminal_disposition",
                    L"content_store_operation_completed");
                auto releaseReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        deadline.Remaining());
                ValidateReservationOperation(
                    releaseReply,
                    leaseBinding,
                    L"RESERVATION_RELEASED",
                    false,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
                auto replayReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        deadline.Remaining());
                ValidateReservationOperation(
                    replayReply,
                    leaseBinding,
                    L"RESERVATION_ALREADY_RELEASED",
                    true,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
            };

            WorkerPersistentStorageBinding storageBinding;
            storageBinding.namespaceId = namespaceId;
            storageBinding.jobId = jobId;
            storageBinding.executionId = executionId;
            storageBinding.datasetId = datasetId;
            storageBinding.executionPlanSha256 =
                executionPlanSha256;
            storageBinding.storagePlanSha256 =
                storagePlanSha256;
            storageBinding.reservationId =
                reservationId;
            storageBinding.generation = generation;
            storageBinding.maximumBytes =
                (std::min<uint64_t>)(
                    domains.declaredPeakBytes,
                    4ull * 1024ull * 1024ull);

            WorkerContentAddressedStoreRecovery storage;
            JsonObject operationResult;
            try
            {
                if (operation == L"core_matrix")
                {
                    operationResult =
                        storage.RunCoreMatrix(
                            workerRoot,
                            storageBinding);
                }
                else if (operation == L"seed_restart")
                {
                    operationResult =
                        storage.SeedRestartFixture(
                            workerRoot,
                            storageBinding);
                }
                else if (operation ==
                    L"recover_restart")
                {
                    auto persistedReservationId =
                        RequiredString(
                            request,
                            L"persisted_reservation_id",
                            L"content_store.request_schema_invalid");
                    auto persistedGeneration =
                        RequiredUInt64(
                            request,
                            L"persisted_generation",
                            L"content_store.request_schema_invalid");
                    if (!IsSafeToken(
                            persistedReservationId,
                            96) ||
                        persistedGeneration == 0 ||
                        persistedGeneration >= generation)
                    {
                        Fail(
                            L"content_store.request_schema_invalid",
                            L"persisted restart binding is invalid");
                    }
                    auto persistedBinding =
                        storageBinding;
                    persistedBinding.reservationId =
                        persistedReservationId;
                    persistedBinding.generation =
                        persistedGeneration;
                    operationResult =
                        storage.RecoverRestartFixture(
                            workerRoot,
                            persistedBinding);
                    PutString(
                        operationResult,
                        L"reauthorized_reservation_id",
                        reservationId);
                    PutNumber(
                        operationResult,
                        L"reauthorized_generation",
                        generation);
                    PutBool(
                        operationResult,
                        L"worker_reauthorization_verified",
                        true);
                }
                else
                {
                    operationResult =
                        storage.CleanupNamespace(
                            workerRoot,
                            namespaceId);
                }
            }
            catch (...)
            {
                try
                {
                    releaseLease();
                }
                catch (...)
                {
                }
                transport.Close();
                throw;
            }
            releaseLease();
            auto closeCompleted = transport.Close();

            JsonObject result;
            PutString(
                result,
                L"schema_version",
                StoreRecoveryResultSchema);
            PutBool(result, L"ok", true);
            PutString(
                result,
                L"delivery_train",
                L"CONTENT_ADDRESSED_STORE_RECOVERY_V1");
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            PutString(
                result,
                L"operation",
                operation);
            PutString(
                result,
                L"storage_plan_sha256",
                storagePlanSha256);
            result.SetNamedValue(
                L"static_admission",
                AdmissionJson(admission));
            JsonObject reservation;
            PutString(
                reservation,
                L"reservation_id",
                reservationId);
            PutBool(
                reservation,
                L"worker_broker_binding_agreed",
                true);
            PutBool(
                reservation,
                L"release_idempotent",
                true);
            PutBool(
                reservation,
                L"client_close_completed",
                closeCompleted);
            result.SetNamedValue(
                L"reservation",
                reservation);
            result.SetNamedValue(
                L"operation_result",
                operationResult);
            JsonObject authority;
            PutBool(
                authority,
                L"worker_semantic",
                true);
            PutBool(
                authority,
                L"worker_final_publication_veto",
                true);
            PutBool(
                authority,
                L"broker_semantic",
                false);
            PutBool(
                authority,
                L"broker_artifact_publication",
                false);
            PutBool(
                authority,
                L"second_artifact_system",
                false);
            result.SetNamedValue(
                L"authority",
                authority);
            return std::wstring(
                result.Stringify().c_str());
        }
        catch (ProcessTopology::Error const& error)
        {
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                StoreRecoveryResultSchema);
            PutBool(result, L"ok", false);
            PutString(
                result,
                L"delivery_train",
                L"CONTENT_ADDRESSED_STORE_RECOVERY_V1");
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            JsonObject detail;
            PutString(
                detail,
                L"code",
                error.code);
            PutString(
                detail,
                L"message",
                error.message);
            PutString(
                detail,
                L"detail",
                error.detail);
            result.SetNamedValue(L"error", detail);
            return std::wstring(
                result.Stringify().c_str());
        }
        catch (WorkerArtifactStoreError const& error)
        {
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                StoreRecoveryResultSchema);
            PutBool(result, L"ok", false);
            PutString(
                result,
                L"delivery_train",
                L"CONTENT_ADDRESSED_STORE_RECOVERY_V1");
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            JsonObject detail;
            PutString(
                detail,
                L"code",
                to_hstring(error.code).c_str());
            PutString(
                detail,
                L"message",
                to_hstring(error.message).c_str());
            PutString(detail, L"detail", L"");
            result.SetNamedValue(L"error", detail);
            return std::wstring(
                result.Stringify().c_str());
        }
        catch (...)
        {
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                StoreRecoveryResultSchema);
            PutBool(result, L"ok", false);
            PutString(
                result,
                L"delivery_train",
                L"CONTENT_ADDRESSED_STORE_RECOVERY_V1");
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            JsonObject detail;
            PutString(
                detail,
                L"code",
                L"content_store.operation_failed");
            PutString(
                detail,
                L"message",
                L"the bounded content-store operation failed");
            PutString(detail, L"detail", L"fail_closed");
            result.SetNamedValue(L"error", detail);
            return std::wstring(
                result.Stringify().c_str());
        }
    }

    std::wstring
    WorkerPersistentComputeCoordinatorRuntime::
        RunProvableWorldsStreamingTiledV1(
            JsonObject const& wireRequest,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot,
            WorkerArtifactStoreConfig const& artifactConfig,
            WorkerArtifactIdGenerator const& idGenerator)
    {
        auto request = JsonObject::Parse(
            wireRequest.Stringify());
        for (auto const* transportField : {
                 L"session_id",
                 L"pairing_code",
                 L"_server_parse_json_ms",
                 L"_server_request_bytes",
                 L"_server_auth_ms" })
        {
            if (request.HasKey(transportField))
            {
                request.Remove(transportField);
            }
        }
        std::wstring correlationId =
            L"provable-worlds-streaming";
        auto errorResult =
            [&](std::wstring const& code,
                std::wstring const& message,
                std::wstring const& detail)
        {
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                WorkerProvableWorldsStreamingResultSchemaVersion);
            PutBool(result, L"ok", false);
            PutString(
                result,
                L"delivery_train",
                WorkerProvableWorldsStreamingTrainId);
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            JsonObject error;
            PutString(error, L"code", code);
            PutString(error, L"message", message);
            PutString(error, L"detail", detail);
            result.SetNamedValue(L"error", error);
            return std::wstring(
                result.Stringify().c_str());
        };

        try
        {
            auto schema = RequiredString(
                request,
                L"schema_version",
                L"t8.request_schema_invalid");
            auto command = RequiredString(
                request,
                L"command",
                L"t8.request_schema_invalid");
            auto operation = RequiredString(
                request,
                L"operation",
                L"t8.request_schema_invalid");
            if (schema !=
                    WorkerProvableWorldsStreamingRequestSchemaVersion ||
                command !=
                    L"run_provable_worlds_streaming_tiled_v1" ||
                (operation != L"uninterrupted" &&
                    operation != L"seed_restart" &&
                    operation != L"resume_restart" &&
                    operation != L"cleanup"))
            {
                Fail(
                    L"t8.request_schema_invalid",
                    L"the T8 command envelope is invalid");
            }
            std::vector<std::wstring> allowedFields{
                L"command",
                L"schema_version",
                L"correlation_id",
                L"operation",
                L"namespace_id",
                L"job_id",
                L"execution_id",
                L"dataset_id",
                L"execution_plan_sha256",
                L"owner_identity",
                L"generation",
                L"persisted_reservation_id",
                L"persisted_generation",
                L"field_artifact_id",
                L"contract_id",
                L"program_id",
                L"program_sha256",
                L"profile_id",
                L"profile_contract_sha256",
                L"bound_input_sha256",
                L"bound_input_mix64",
                L"expected_field_sha256",
                L"tile_width",
                L"tile_height",
                L"tile_depth",
                L"tile_lanes",
                L"tile_bytes",
                L"tile_count",
                L"field_bytes",
                L"schedule",
                L"output_double_buffered",
                L"stop_before_commit_tile_index",
                L"retention_mix",
                L"maximum_simultaneous_buffers",
                L"protected_recovery_pinned_bytes",
                L"evictable_cache_ephemeral_bytes",
                L"persistent_artifact_bytes",
                L"metadata_journal_bytes",
                L"temporary_bytes",
                L"declared_peak_bytes",
                L"declared_physical_growth_bytes",
                L"platform_available_known",
                L"platform_available_bytes"
            };
            for (auto const& field : request)
            {
                auto name =
                    std::wstring(field.Key().c_str());
                if (std::find(
                        allowedFields.begin(),
                        allowedFields.end(),
                        name) ==
                    allowedFields.end())
                {
                    Fail(
                        L"t8.request_schema_invalid",
                        L"the strict T8 request contains an unknown field",
                        name);
                }
            }
            if (!RequiredBool(
                    request,
                    L"output_double_buffered",
                    L"t8.request_schema_invalid") ||
                (operation == L"resume_restart") !=
                    request.HasKey(
                        L"persisted_reservation_id") ||
                (operation == L"resume_restart") !=
                    request.HasKey(
                        L"persisted_generation"))
            {
                Fail(
                    L"t8.request_schema_invalid",
                    L"the T8 buffering or restart conditional contract is invalid");
            }
            correlationId = RequiredString(
                request,
                L"correlation_id",
                L"t8.request_schema_invalid");
            auto namespaceId = RequiredString(
                request,
                L"namespace_id",
                L"t8.request_schema_invalid");
            auto jobId = RequiredString(
                request,
                L"job_id",
                L"t8.request_schema_invalid");
            auto executionId = RequiredString(
                request,
                L"execution_id",
                L"t8.request_schema_invalid");
            auto datasetId = RequiredString(
                request,
                L"dataset_id",
                L"t8.request_schema_invalid");
            auto executionPlanSha256 = RequiredString(
                request,
                L"execution_plan_sha256",
                L"t8.request_schema_invalid");
            auto ownerIdentity = RequiredString(
                request,
                L"owner_identity",
                L"t8.request_schema_invalid");
            auto fieldArtifactId = RequiredString(
                request,
                L"field_artifact_id",
                L"t8.request_schema_invalid");
            auto retentionMix = RequiredString(
                request,
                L"retention_mix",
                L"t8.request_schema_invalid");
            if (!IsSafeToken(correlationId, 64) ||
                !IsSafeToken(namespaceId, 40) ||
                !IsSafeToken(jobId, 64) ||
                !IsSafeToken(executionId, 64) ||
                !IsSafeToken(datasetId, 64) ||
                !IsSafeToken(ownerIdentity, 96) ||
                !IsSafeToken(fieldArtifactId, 96) ||
                !IsLowerHex(executionPlanSha256, 64) ||
                retentionMix !=
                    L"EPHEMERAL_RECOVERY_PINNED")
            {
                Fail(
                    L"t8.request_schema_invalid",
                    L"one or more T8 identity fields are invalid");
            }

            auto requireExactString =
                [&](wchar_t const* name,
                    wchar_t const* expected)
            {
                auto actual = RequiredString(
                    request,
                    name,
                    L"t8.request_schema_invalid");
                if (actual != expected)
                {
                    Fail(
                        L"t8.request_contract_mismatch",
                        L"a frozen T8 content identity changed",
                        name);
                }
            };
            requireExactString(
                L"contract_id",
                L"PROVABLE_WORLDS_STREAMING_PRODUCT_ADMISSION_V1");
            requireExactString(
                L"program_id",
                L"provable_worlds_field_v1");
            requireExactString(
                L"program_sha256",
                L"5c8c06aa8700f0dcec90263c5a4243cd4ecfebbaaec25e870074e1906e8fc836");
            requireExactString(
                L"profile_id",
                L"provable_worlds_cpu_capsule_v1");
            requireExactString(
                L"profile_contract_sha256",
                L"e0b6a59c07b7225135efb5fce4b0f72f18ddb903325fdbdf35b038415f366486");
            requireExactString(
                L"bound_input_sha256",
                L"3eecbcff36b8542da2fd554693ed3dc349c9e0a381adb42c70541a9a816b81d1");
            requireExactString(
                L"bound_input_mix64",
                L"60b1b806ed391db2");
            requireExactString(
                L"expected_field_sha256",
                L"24cd52a3e9b9f2014ba020a9fdbaecce7a4604a6ee47a336739484d168458bef");
            requireExactString(
                L"schedule",
                L"prefetch N+1 -> compute N -> commit N-1");

            auto requireExactNumber =
                [&](wchar_t const* name,
                    uint64_t expected)
            {
                auto actual = RequiredUInt64(
                    request,
                    name,
                    L"t8.request_schema_invalid");
                if (actual != expected)
                {
                    Fail(
                        L"t8.request_contract_mismatch",
                        L"the frozen T8 geometry changed",
                        name);
                }
            };
            requireExactNumber(L"tile_width", 1024);
            requireExactNumber(L"tile_height", 4);
            requireExactNumber(L"tile_depth", 1);
            requireExactNumber(L"tile_lanes", 4096);
            requireExactNumber(L"tile_bytes", 16384);
            requireExactNumber(L"tile_count", 256);
            requireExactNumber(L"field_bytes", 4194304);
            requireExactNumber(
                L"maximum_simultaneous_buffers",
                4);
            requireExactNumber(
                L"stop_before_commit_tile_index",
                128);

            XComputeShared::XcpStorageQuotaDomains domains;
            domains.protectedRecoveryPinnedBytes =
                RequiredUInt64(
                    request,
                    L"protected_recovery_pinned_bytes",
                    L"t8.request_schema_invalid");
            domains.evictableCacheEphemeralBytes =
                RequiredUInt64(
                    request,
                    L"evictable_cache_ephemeral_bytes",
                    L"t8.request_schema_invalid");
            domains.persistentArtifactBytes =
                RequiredUInt64(
                    request,
                    L"persistent_artifact_bytes",
                    L"t8.request_schema_invalid");
            domains.metadataJournalBytes =
                RequiredUInt64(
                    request,
                    L"metadata_journal_bytes",
                    L"t8.request_schema_invalid");
            domains.temporaryBytes =
                RequiredUInt64(
                    request,
                    L"temporary_bytes",
                    L"t8.request_schema_invalid");
            domains.declaredPeakBytes =
                RequiredUInt64(
                    request,
                    L"declared_peak_bytes",
                    L"t8.request_schema_invalid");
            domains.declaredPhysicalGrowthBytes =
                RequiredUInt64(
                    request,
                    L"declared_physical_growth_bytes",
                    L"t8.request_schema_invalid");
            domains.platformAvailableKnown =
                RequiredBool(
                    request,
                    L"platform_available_known",
                    L"t8.request_schema_invalid");
            domains.platformAvailableBytes =
                RequiredUInt64(
                    request,
                    L"platform_available_bytes",
                    L"t8.request_schema_invalid");
            domains.maximumSimultaneousBuffers = 4;
            auto generation = RequiredUInt64(
                request,
                L"generation",
                L"t8.request_schema_invalid");
            if (generation == 0)
            {
                Fail(
                    L"t8.request_schema_invalid",
                    L"generation must be nonzero");
            }

            auto policy =
                XComputeShared::XcpDefaultStorageQuotaPolicy();
            auto canonicalPlan =
                std::wstring(
                    XComputeShared::XcpCoordinatorPolicyVersion) +
                L"|logical_page_bytes=16384" +
                L"|page_count=256" +
                L"|journal_records=257" +
                L"|declared_peak_bytes=" +
                std::to_wstring(domains.declaredPeakBytes) +
                L"|retention_mix=" + retentionMix +
                L"|maximum_simultaneous_buffers=4" +
                L"|execution_plan_sha256=" +
                executionPlanSha256 +
                L"|contract_id=PROVABLE_WORLDS_STREAMING_PRODUCT_ADMISSION_V1";
            auto storagePlanSha256 =
                Sha256(canonicalPlan);
            auto admission =
                XComputeShared::XcpEvaluateStorageAdmission(
                    policy,
                    domains,
                    0);
            if (!admission.admitted)
            {
                Fail(
                    L"t8.static_admission_refused",
                    L"the T8 storage plan failed static admission",
                    admission.code);
            }

            XComputeShared::XcpReservationBinding
                leaseBinding;
            leaseBinding.jobId = jobId;
            leaseBinding.executionId = executionId;
            leaseBinding.storagePlanSha256 =
                storagePlanSha256;
            leaseBinding.declaredPeakBytes =
                domains.declaredPeakBytes;
            leaseBinding.generation = generation;
            leaseBinding.ownerIdentity = ownerIdentity;
            leaseBinding.quotaDomains = domains;

            MonotonicDeadline deadline;
            WorkerProcessTopologyTransport transport;
            auto family =
                Package::Current().Id().FamilyName();
            auto opened = transport.Open(
                AppServiceName,
                std::wstring_view(family),
                deadline.Remaining());
            if (!opened.succeeded)
            {
                Fail(
                    L"t8.broker_open_failed",
                    L"the Case B reservation coordinator did not activate",
                    std::to_wstring(opened.statusCode));
            }
            auto descriptionReply =
                SendBrokerCommand(
                    transport,
                    NewBrokerCommand(
                        L"coordinator_describe"),
                    deadline.Remaining());
            ValidateDescription(
                descriptionReply.result,
                policy);

            std::wstring persistedReservationId;
            uint64_t persistedGeneration = 0;
            if (operation == L"resume_restart")
            {
                persistedReservationId = RequiredString(
                    request,
                    L"persisted_reservation_id",
                    L"t8.request_schema_invalid");
                persistedGeneration = RequiredUInt64(
                    request,
                    L"persisted_generation",
                    L"t8.request_schema_invalid");
                if (!IsSafeToken(
                        persistedReservationId,
                        96) ||
                    persistedGeneration == 0 ||
                    persistedGeneration >= generation)
                {
                    Fail(
                        L"t8.request_schema_invalid",
                        L"the persisted restart binding must precede the fresh lease generation");
                }
            }
            auto acquireRequest =
                NewBrokerCommand(
                    L"reservation_acquire");
            AttachBinding(
                acquireRequest,
                leaseBinding);
            auto acquireReply =
                SendBrokerCommand(
                    transport,
                    acquireRequest,
                    deadline.Remaining());
            auto acquired =
                ValidateReservationOperation(
                    acquireReply,
                    leaseBinding,
                    L"RESERVATION_ACQUIRED",
                    false,
                    L"active",
                    { L"requested", L"active" });
            auto reservationId = RequiredString(
                acquired,
                L"reservation_id",
                L"t8.broker_response_invalid");

            bool leaseReleased = false;
            bool releaseIdempotent = false;
            auto releaseLease = [&]()
            {
                auto releaseRequest =
                    NewBrokerCommand(
                        L"reservation_release");
                AttachBinding(
                    releaseRequest,
                    leaseBinding);
                PutString(
                    releaseRequest,
                    L"reservation_id",
                    reservationId);
                PutString(
                    releaseRequest,
                    L"terminal_disposition",
                    L"t8_streaming_operation_completed");
                auto releaseReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        deadline.Remaining());
                ValidateReservationOperation(
                    releaseReply,
                    leaseBinding,
                    L"RESERVATION_RELEASED",
                    false,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
                auto replayReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        deadline.Remaining());
                ValidateReservationOperation(
                    replayReply,
                    leaseBinding,
                    L"RESERVATION_ALREADY_RELEASED",
                    true,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
                leaseReleased = true;
                releaseIdempotent = true;
            };

            WorkerPersistentStorageBinding
                storageBinding;
            storageBinding.namespaceId = namespaceId;
            storageBinding.jobId = jobId;
            storageBinding.executionId = executionId;
            storageBinding.datasetId = datasetId;
            storageBinding.executionPlanSha256 =
                executionPlanSha256;
            storageBinding.storagePlanSha256 =
                storagePlanSha256;
            storageBinding.reservationId =
                reservationId;
            storageBinding.generation = generation;
            storageBinding.maximumBytes =
                4ull * 1024ull * 1024ull;
            storageBinding.logicalPageBytes = 16384;
            storageBinding.maximumPageCount = 256;
            storageBinding.maximumJournalRecords = 257;
            WorkerProvableWorldsRestartAuthorization
                restartAuthorization;
            WorkerProvableWorldsRestartAuthorization const*
                restartAuthorizationPointer = nullptr;
            if (operation == L"resume_restart")
            {
                storageBinding.reservationId =
                    persistedReservationId;
                storageBinding.generation =
                    persistedGeneration;
                restartAuthorization.persistedReservationId =
                    persistedReservationId;
                restartAuthorization.persistedGeneration =
                    persistedGeneration;
                restartAuthorization.activeReservationId =
                    reservationId;
                restartAuthorization.activeGeneration =
                    generation;
                restartAuthorization.workerBrokerBindingAgreed =
                    true;
                restartAuthorizationPointer =
                    &restartAuthorization;
            }

            JsonObject operationResult;
            try
            {
                if (operation == L"cleanup")
                {
                    WorkerContentAddressedStoreRecovery
                        storage;
                    auto storageCleanup =
                        storage.CleanupNamespace(
                            workerRoot,
                            namespaceId);
                    JsonObject deleteRequest;
                    PutString(
                        deleteRequest,
                        L"artifact_id",
                        fieldArtifactId);
                    PutBool(
                        deleteRequest,
                        L"delete_unreferenced_blob",
                        true);
                    auto artifactCleanup =
                        JsonObject::Parse(
                            hstring(
                                WorkerExecuteDeleteArtifact(
                                    deleteRequest,
                                    workerRoot,
                                    artifactConfig)));
                    PutString(
                        operationResult,
                        L"schema_version",
                        L"xcp-provable-worlds-streaming-tiled-v1-cleanup-result");
                    PutString(
                        operationResult,
                        L"operation",
                        L"cleanup");
                    PutBool(
                        operationResult,
                        L"ok",
                        storageCleanup.GetNamedBoolean(
                            L"namespace_absent"));
                    PutBool(
                        operationResult,
                        L"namespace_existed",
                        storageCleanup.GetNamedBoolean(
                            L"namespace_existed"));
                    PutNumber(
                        operationResult,
                        L"removed_namespace_entries",
                        RequiredUInt64(
                            storageCleanup,
                            L"removed_entries",
                            L"t8.cleanup_result_invalid"));
                    PutNumber(
                        operationResult,
                        L"removed_unreferenced_page_blobs",
                        RequiredUInt64(
                            storageCleanup,
                            L"removed_unreferenced_blobs",
                            L"t8.cleanup_result_invalid"));
                    PutBool(
                        operationResult,
                        L"namespace_absent",
                        storageCleanup.GetNamedBoolean(
                            L"namespace_absent"));
                    PutBool(
                        operationResult,
                        L"artifact_existed",
                        artifactCleanup.GetNamedBoolean(
                            L"existed"));
                    PutBool(
                        operationResult,
                        L"artifact_manifest_removed",
                        artifactCleanup.GetNamedBoolean(
                            L"manifest_removed"));
                    PutBool(
                        operationResult,
                        L"artifact_blob_removed",
                        artifactCleanup.GetNamedBoolean(
                            L"blob_removed"));
                    PutNumber(
                        operationResult,
                        L"artifact_removed_bytes_estimate",
                        RequiredUInt64(
                            artifactCleanup,
                            L"removed_bytes_estimate",
                            L"t8.cleanup_result_invalid"));
                    auto cleanupBytes =
                        RequiredUInt64(
                            artifactCleanup,
                            L"removed_bytes_estimate",
                            L"t8.cleanup_result_invalid") +
                        RequiredUInt64(
                            storageCleanup,
                            L"removed_unreferenced_blobs",
                            L"t8.cleanup_result_invalid") *
                            16384;
                    PutNumber(
                        operationResult,
                        L"cleanup_bytes_estimate",
                        cleanupBytes);
                    PutBool(
                        operationResult,
                        L"final_workspace_storage_invariant",
                        storageCleanup.GetNamedBoolean(
                            L"namespace_absent") &&
                        !std::filesystem::exists(
                            ArtifactManifestPath(
                                workerRoot,
                                fieldArtifactId)) &&
                        !std::filesystem::exists(
                            ArtifactStagingDirectory(
                                workerRoot,
                                fieldArtifactId)));
                }
                else
                {
                    WorkerProvableWorldsStreamingRuntime
                        runtime;
                    operationResult = runtime.Execute(
                        workerRoot,
                        storageBinding,
                        restartAuthorizationPointer,
                        operation,
                        fieldArtifactId,
                        artifactConfig,
                        idGenerator);
                }
            }
            catch (...)
            {
                try
                {
                    releaseLease();
                }
                catch (...)
                {
                }
                transport.Close();
                throw;
            }

            auto retainLease =
                operation == L"seed_restart";
            if (!retainLease)
            {
                releaseLease();
            }
            auto closeCompleted = transport.Close();

            JsonObject result;
            PutString(
                result,
                L"schema_version",
                WorkerProvableWorldsStreamingResultSchemaVersion);
            PutBool(result, L"ok", true);
            PutString(
                result,
                L"delivery_train",
                WorkerProvableWorldsStreamingTrainId);
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            PutString(
                result,
                L"operation",
                operation);
            PutString(
                result,
                L"storage_plan_sha256",
                storagePlanSha256);
            result.SetNamedValue(
                L"static_admission",
                AdmissionJson(admission));
            JsonObject reservation;
            PutString(
                reservation,
                L"reservation_id",
                reservationId);
            PutNumber(
                reservation,
                L"generation",
                generation);
            PutNumber(
                reservation,
                L"declared_peak_bytes",
                domains.declaredPeakBytes);
            PutBool(
                reservation,
                L"worker_broker_binding_agreed",
                true);
            PutBool(
                reservation,
                L"fresh_lease_acquired",
                true);
            PutBool(
                reservation,
                L"lease_active_at_seed_boundary",
                retainLease);
            PutBool(
                reservation,
                L"lease_released",
                leaseReleased);
            PutBool(
                reservation,
                L"release_idempotent",
                releaseIdempotent);
            PutBool(
                reservation,
                L"client_close_completed",
                closeCompleted);
            if (operation == L"resume_restart")
            {
                JsonObject reauthorization;
                PutString(
                    reauthorization,
                    L"persisted_reservation_id",
                    persistedReservationId);
                PutNumber(
                    reauthorization,
                    L"persisted_generation",
                    persistedGeneration);
                PutBool(
                    reauthorization,
                    L"existing_active_lease_resumed",
                    false);
                PutBool(
                    reauthorization,
                    L"new_generation_strictly_greater",
                    generation > persistedGeneration);
                PutBool(
                    reauthorization,
                    L"worker_reauthorization_verified",
                    true);
                reservation.SetNamedValue(
                    L"restart_reauthorization",
                    reauthorization);
            }
            result.SetNamedValue(
                L"reservation",
                reservation);
            result.SetNamedValue(
                L"operation_result",
                operationResult);
            JsonObject authority;
            PutBool(
                authority,
                L"worker_semantic",
                true);
            PutBool(
                authority,
                L"worker_final_publication_veto",
                true);
            PutBool(
                authority,
                L"broker_semantic",
                false);
            PutBool(
                authority,
                L"broker_artifact_publication",
                false);
            PutBool(
                authority,
                L"second_backend",
                false);
            PutBool(
                authority,
                L"second_artifact_system",
                false);
            result.SetNamedValue(
                L"authority",
                authority);
            return std::wstring(
                result.Stringify().c_str());
        }
        catch (ProcessTopology::Error const& error)
        {
            return errorResult(
                error.code,
                error.message,
                error.detail);
        }
        catch (WorkerProvableWorldsStreamingError const& error)
        {
            return errorResult(
                error.code,
                to_hstring(error.message).c_str(),
                L"fail_closed");
        }
        catch (WorkerArtifactStoreError const& error)
        {
            return errorResult(
                to_hstring(error.code).c_str(),
                to_hstring(error.message).c_str(),
                L"fail_closed");
        }
        catch (hresult_error const& error)
        {
            return errorResult(
                L"t8.platform_failure",
                L"a packaged platform API failed",
                std::to_wstring(error.code().value));
        }
        catch (...)
        {
            return errorResult(
                L"t8.unexpected_failure",
                L"an unexpected bounded T8 failure occurred",
                L"fail_closed");
        }
    }

    std::wstring
    WorkerPersistentComputeCoordinatorRuntime::
        RunStorageScaleCharacterizationV1(
            JsonObject const& wireRequest,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot)
    {
        constexpr uint64_t MiB = 1024ull * 1024ull;
        constexpr uint64_t GiB = 1024ull * MiB;
        constexpr uint64_t PageBytes = 4ull * MiB;
        constexpr uint64_t SeedBytes = 8ull * GiB;
        constexpr uint64_t TargetBytes = 8ull * GiB;
        constexpr uint64_t TargetPageCount =
            TargetBytes / PageBytes;

        auto request = JsonObject::Parse(
            wireRequest.Stringify());
        for (auto const* transportField : {
                 L"session_id",
                 L"pairing_code",
                 L"_server_parse_json_ms",
                 L"_server_request_bytes",
                 L"_server_auth_ms" })
        {
            if (request.HasKey(transportField))
            {
                request.Remove(transportField);
            }
        }
        std::wstring correlationId =
            L"storage-scale-characterization";
        auto errorResult =
            [&](std::wstring const& code,
                std::wstring const& message,
                std::wstring const& detail)
        {
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                WorkerStorageScaleCharacterizationResultSchemaVersion);
            PutBool(result, L"ok", false);
            PutString(
                result,
                L"delivery_train",
                WorkerStorageScaleCharacterizationTrainId);
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            JsonObject error;
            PutString(error, L"code", code);
            PutString(error, L"message", message);
            PutString(error, L"detail", detail);
            result.SetNamedValue(L"error", error);
            return std::wstring(
                result.Stringify().c_str());
        };

        try
        {
            auto schema = RequiredString(
                request,
                L"schema_version",
                L"storage_scale.request_schema_invalid");
            auto command = RequiredString(
                request,
                L"command",
                L"storage_scale.request_schema_invalid");
            auto operation = RequiredString(
                request,
                L"operation",
                L"storage_scale.request_schema_invalid");
            if (schema !=
                    WorkerStorageScaleCharacterizationRequestSchemaVersion ||
                command !=
                    L"run_storage_scale_characterization_v1" ||
                (operation != L"seed" &&
                 operation != L"resume" &&
                 operation != L"verify" &&
                 operation != L"cleanup"))
            {
                Fail(
                    L"storage_scale.request_schema_invalid",
                    L"the storage scale command envelope is invalid");
            }

            std::vector<std::wstring> allowedFields{
                L"command",
                L"schema_version",
                L"correlation_id",
                L"operation",
                L"namespace_id",
                L"job_id",
                L"execution_id",
                L"dataset_id",
                L"execution_plan_sha256",
                L"owner_identity",
                L"generation",
                L"persisted_reservation_id",
                L"persisted_generation"
            };
            for (auto const& field : request)
            {
                auto name =
                    std::wstring(field.Key().c_str());
                if (std::find(
                        allowedFields.begin(),
                        allowedFields.end(),
                        name) ==
                    allowedFields.end())
                {
                    Fail(
                        L"storage_scale.request_schema_invalid",
                        L"the strict storage scale request contains an unknown field",
                        name);
                }
            }

            auto restartOperation =
                operation != L"seed";
            if (restartOperation !=
                    request.HasKey(
                        L"persisted_reservation_id") ||
                restartOperation !=
                    request.HasKey(
                        L"persisted_generation"))
            {
                Fail(
                    L"storage_scale.request_schema_invalid",
                    L"restart authorization fields do not match the requested operation");
            }

            correlationId = RequiredString(
                request,
                L"correlation_id",
                L"storage_scale.request_schema_invalid");
            auto namespaceId = RequiredString(
                request,
                L"namespace_id",
                L"storage_scale.request_schema_invalid");
            auto jobId = RequiredString(
                request,
                L"job_id",
                L"storage_scale.request_schema_invalid");
            auto executionId = RequiredString(
                request,
                L"execution_id",
                L"storage_scale.request_schema_invalid");
            auto datasetId = RequiredString(
                request,
                L"dataset_id",
                L"storage_scale.request_schema_invalid");
            auto executionPlanSha256 = RequiredString(
                request,
                L"execution_plan_sha256",
                L"storage_scale.request_schema_invalid");
            auto ownerIdentity = RequiredString(
                request,
                L"owner_identity",
                L"storage_scale.request_schema_invalid");
            auto generation = RequiredUInt64(
                request,
                L"generation",
                L"storage_scale.request_schema_invalid");
            if (!IsSafeToken(correlationId, 64) ||
                !IsSafeToken(namespaceId, 40) ||
                !IsSafeToken(jobId, 64) ||
                !IsSafeToken(executionId, 64) ||
                !IsSafeToken(datasetId, 64) ||
                !IsSafeToken(ownerIdentity, 96) ||
                !IsLowerHex(executionPlanSha256, 64) ||
                generation == 0)
            {
                Fail(
                    L"storage_scale.request_schema_invalid",
                    L"one or more storage scale identity fields are invalid");
            }

            auto policy =
                XComputeShared::XcpDefaultStorageQuotaPolicy();
            auto canonicalPlan =
                std::wstring(
                    XComputeShared::XcpCoordinatorPolicyVersion) +
                L"|contract=STORAGE_SCALE_CHARACTERIZATION_V1" +
                L"|target_bytes=8589934592" +
                L"|seed_bytes=8589934592" +
                L"|logical_page_bytes=4194304" +
                L"|page_count=2048" +
                L"|journal_records=2049" +
                L"|capacity_ladder_bytes=268435456,536870912,1073741824,2147483648,4294967296,8589934592" +
                L"|safety_reserve_bytes=17179869184" +
                L"|retention_mix=RECOVERY_PINNED" +
                L"|maximum_simultaneous_buffers=2" +
                L"|execution_plan_sha256=" +
                executionPlanSha256;
            auto storagePlanSha256 =
                Sha256(canonicalPlan);

            XComputeShared::XcpStorageQuotaDomains domains;
            domains.maximumSimultaneousBuffers =
                operation == L"verify" ||
                operation == L"cleanup"
                    ? 1
                    : 2;
            if (operation == L"seed")
            {
                domains.declaredPeakBytes = SeedBytes;
                domains.declaredPhysicalGrowthBytes =
                    SeedBytes;
            }
            else if (operation == L"resume")
            {
                domains.protectedRecoveryPinnedBytes =
                    SeedBytes;
                domains.declaredPeakBytes = PageBytes;
                domains.declaredPhysicalGrowthBytes =
                    TargetBytes - SeedBytes;
            }
            else
            {
                domains.protectedRecoveryPinnedBytes =
                    TargetBytes;
                domains.declaredPeakBytes = PageBytes;
            }
            auto admission =
                XComputeShared::XcpEvaluateStorageAdmission(
                    policy,
                    domains,
                    0);
            if (!admission.admitted)
            {
                Fail(
                    L"storage_scale.static_admission_refused",
                    L"the storage scale plan failed static admission",
                    admission.code);
            }

            std::wstring persistedReservationId;
            uint64_t persistedGeneration = 0;
            if (restartOperation)
            {
                persistedReservationId = RequiredString(
                    request,
                    L"persisted_reservation_id",
                    L"storage_scale.request_schema_invalid");
                persistedGeneration = RequiredUInt64(
                    request,
                    L"persisted_generation",
                    L"storage_scale.request_schema_invalid");
                if (!IsSafeToken(
                        persistedReservationId,
                        96) ||
                    persistedGeneration == 0 ||
                    persistedGeneration >= generation)
                {
                    Fail(
                        L"storage_scale.request_schema_invalid",
                        L"the persisted binding must precede the fresh lease generation");
                }
            }

            XComputeShared::XcpReservationBinding
                leaseBinding;
            leaseBinding.jobId = jobId;
            leaseBinding.executionId = executionId;
            leaseBinding.storagePlanSha256 =
                storagePlanSha256;
            leaseBinding.declaredPeakBytes =
                domains.declaredPeakBytes;
            leaseBinding.generation = generation;
            leaseBinding.ownerIdentity = ownerIdentity;
            leaseBinding.quotaDomains = domains;

            MonotonicDeadline deadline;
            WorkerProcessTopologyTransport transport;
            auto family =
                Package::Current().Id().FamilyName();
            auto opened = transport.Open(
                AppServiceName,
                std::wstring_view(family),
                deadline.Remaining());
            if (!opened.succeeded)
            {
                Fail(
                    L"storage_scale.broker_open_failed",
                    L"the Case B reservation coordinator did not activate",
                    std::to_wstring(opened.statusCode));
            }
            auto descriptionReply =
                SendBrokerCommand(
                    transport,
                    NewBrokerCommand(
                        L"coordinator_describe"),
                    deadline.Remaining());
            ValidateDescription(
                descriptionReply.result,
                policy);

            auto acquireRequest =
                NewBrokerCommand(
                    L"reservation_acquire");
            AttachBinding(
                acquireRequest,
                leaseBinding);
            auto acquireReply =
                SendBrokerCommand(
                    transport,
                    acquireRequest,
                    deadline.Remaining());
            auto acquired =
                ValidateReservationOperation(
                    acquireReply,
                    leaseBinding,
                    L"RESERVATION_ACQUIRED",
                    false,
                    L"active",
                    { L"requested", L"active" });
            auto reservationId = RequiredString(
                acquired,
                L"reservation_id",
                L"storage_scale.broker_response_invalid");

            bool leaseReleased = false;
            bool releaseIdempotent = false;
            auto releaseLease = [&]()
            {
                MonotonicDeadline releaseDeadline;
                auto releaseRequest =
                    NewBrokerCommand(
                        L"reservation_release");
                AttachBinding(
                    releaseRequest,
                    leaseBinding);
                PutString(
                    releaseRequest,
                    L"reservation_id",
                    reservationId);
                PutString(
                    releaseRequest,
                    L"terminal_disposition",
                    L"storage_scale_operation_completed");
                auto releaseReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        releaseDeadline.Remaining());
                ValidateReservationOperation(
                    releaseReply,
                    leaseBinding,
                    L"RESERVATION_RELEASED",
                    false,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
                auto replayReply =
                    SendBrokerCommand(
                        transport,
                        releaseRequest,
                        releaseDeadline.Remaining());
                ValidateReservationOperation(
                    replayReply,
                    leaseBinding,
                    L"RESERVATION_ALREADY_RELEASED",
                    true,
                    L"released",
                    {
                        L"requested",
                        L"active",
                        L"releasing",
                        L"released"
                    });
                leaseReleased = true;
                releaseIdempotent = true;
            };

            WorkerPersistentStorageBinding storageBinding;
            storageBinding.namespaceId = namespaceId;
            storageBinding.jobId = jobId;
            storageBinding.executionId = executionId;
            storageBinding.datasetId = datasetId;
            storageBinding.executionPlanSha256 =
                executionPlanSha256;
            storageBinding.storagePlanSha256 =
                storagePlanSha256;
            storageBinding.reservationId =
                restartOperation
                    ? persistedReservationId
                    : reservationId;
            storageBinding.generation =
                restartOperation
                    ? persistedGeneration
                    : generation;
            storageBinding.maximumBytes = TargetBytes;
            storageBinding.logicalPageBytes = PageBytes;
            storageBinding.maximumPageCount =
                TargetPageCount;
            storageBinding.maximumJournalRecords =
                TargetPageCount + 1;

            WorkerStorageScaleRestartAuthorization
                restartAuthorization;
            WorkerStorageScaleRestartAuthorization const*
                restartAuthorizationPointer = nullptr;
            if (restartOperation)
            {
                restartAuthorization.persistedReservationId =
                    persistedReservationId;
                restartAuthorization.persistedGeneration =
                    persistedGeneration;
                restartAuthorization.activeReservationId =
                    reservationId;
                restartAuthorization.activeGeneration =
                    generation;
                restartAuthorization.workerBrokerBindingAgreed =
                    true;
                restartAuthorizationPointer =
                    &restartAuthorization;
            }

            JsonObject operationResult;
            try
            {
                WorkerStorageScaleCharacterizationRuntime
                    runtime;
                operationResult = runtime.Execute(
                    workerRoot,
                    storageBinding,
                    restartAuthorizationPointer,
                    operation);
                if (!operationResult.GetNamedBoolean(
                        L"ok",
                        false))
                {
                    Fail(
                        L"storage_scale.operation_failed",
                        L"the storage scale runtime did not satisfy its operation contract");
                }
            }
            catch (...)
            {
                try
                {
                    releaseLease();
                }
                catch (...)
                {
                }
                transport.Close();
                throw;
            }

            releaseLease();
            auto closeCompleted = transport.Close();

            JsonObject result;
            PutString(
                result,
                L"schema_version",
                WorkerStorageScaleCharacterizationResultSchemaVersion);
            PutBool(result, L"ok", true);
            PutString(
                result,
                L"delivery_train",
                WorkerStorageScaleCharacterizationTrainId);
            PutString(
                result,
                L"protocol_version",
                protocolVersion);
            PutString(
                result,
                L"correlation_id",
                correlationId);
            PutString(
                result,
                L"operation",
                operation);
            PutString(
                result,
                L"storage_plan_sha256",
                storagePlanSha256);
            result.SetNamedValue(
                L"policy",
                PolicyJson(policy));
            result.SetNamedValue(
                L"static_admission",
                AdmissionJson(admission));

            JsonObject reservation;
            PutString(
                reservation,
                L"reservation_id",
                reservationId);
            PutNumber(
                reservation,
                L"generation",
                generation);
            PutNumber(
                reservation,
                L"declared_peak_bytes",
                domains.declaredPeakBytes);
            PutBool(
                reservation,
                L"fresh_lease_acquired",
                true);
            PutBool(
                reservation,
                L"worker_broker_binding_agreed",
                true);
            PutBool(
                reservation,
                L"lease_released",
                leaseReleased);
            PutBool(
                reservation,
                L"release_idempotent",
                releaseIdempotent);
            PutBool(
                reservation,
                L"client_close_completed",
                closeCompleted);
            if (restartOperation)
            {
                JsonObject reauthorization;
                PutString(
                    reauthorization,
                    L"persisted_reservation_id",
                    persistedReservationId);
                PutNumber(
                    reauthorization,
                    L"persisted_generation",
                    persistedGeneration);
                PutBool(
                    reauthorization,
                    L"existing_active_lease_resumed",
                    false);
                PutBool(
                    reauthorization,
                    L"new_generation_strictly_greater",
                    generation > persistedGeneration);
                PutBool(
                    reauthorization,
                    L"worker_reauthorization_verified",
                    true);
                reservation.SetNamedValue(
                    L"restart_reauthorization",
                    reauthorization);
            }
            result.SetNamedValue(
                L"reservation",
                reservation);
            result.SetNamedValue(
                L"operation_result",
                operationResult);

            JsonObject authority;
            PutBool(
                authority,
                L"worker_semantic",
                true);
            PutBool(
                authority,
                L"worker_final_publication_veto",
                true);
            PutBool(
                authority,
                L"broker_semantic",
                false);
            PutBool(
                authority,
                L"second_backend",
                false);
            PutBool(
                authority,
                L"second_artifact_system",
                false);
            result.SetNamedValue(
                L"authority",
                authority);
            return std::wstring(
                result.Stringify().c_str());
        }
        catch (ProcessTopology::Error const& error)
        {
            return errorResult(
                error.code,
                error.message,
                error.detail);
        }
        catch (
            WorkerStorageScaleCharacterizationError const& error)
        {
            return errorResult(
                error.code,
                to_hstring(error.message).c_str(),
                L"fail_closed");
        }
        catch (WorkerArtifactStoreError const& error)
        {
            return errorResult(
                to_hstring(error.code).c_str(),
                to_hstring(error.message).c_str(),
                L"fail_closed");
        }
        catch (hresult_error const& error)
        {
            return errorResult(
                L"storage_scale.platform_failure",
                L"a packaged platform API failed",
                std::to_wstring(error.code().value));
        }
        catch (...)
        {
            return errorResult(
                L"storage_scale.unexpected_failure",
                L"an unexpected bounded storage scale failure occurred",
                L"fail_closed");
        }
    }

    std::wstring WorkerPersistentComputeCoordinatorCapabilityJson()
    {
        auto policy =
            XComputeShared::XcpDefaultStorageQuotaPolicy();
        JsonObject value;
        PutString(
            value,
            L"schema_version",
            L"xcp-persistent-compute-coordinator-capability-v1");
        PutString(
            value,
            L"delivery_train",
            XComputeShared::XcpCoordinatorTrainId);
        PutString(
            value,
            L"request_schema",
            CoordinatorRequestSchema);
        PutString(
            value,
            L"result_schema",
            CoordinatorResultSchema);
        PutString(
            value,
            L"selected_topology",
            L"CASE_B_OUT_OF_PROCESS_NARROW_COORDINATOR");
        PutBool(
            value,
            L"single_process_adapter_available",
            true);
        PutBool(value, L"page_store_materialized", false);
        PutBool(
            value,
            L"persistent_journal_materialized",
            false);
        value.SetNamedValue(L"policy", PolicyJson(policy));
        return std::wstring(value.Stringify().c_str());
    }
}
