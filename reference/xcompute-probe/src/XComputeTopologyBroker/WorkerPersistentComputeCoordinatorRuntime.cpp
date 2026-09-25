#include "pch.h"
#include "WorkerPersistentComputeCoordinatorRuntime.h"

#include "WorkerProcessTopologyBrokerProtocol.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace winrt::XComputeTopologyBroker::implementation
{
    namespace
    {
        bool IsSafeToken(
            std::wstring const& value,
            size_t maximumLength)
        {
            return
                !value.empty() &&
                value.size() <= maximumLength &&
                std::all_of(
                    value.begin(),
                    value.end(),
                    [](wchar_t ch)
                    {
                        return
                            (ch >= L'a' && ch <= L'z') ||
                            (ch >= L'A' && ch <= L'Z') ||
                            (ch >= L'0' && ch <= L'9') ||
                            ch == L'.' || ch == L'_' ||
                            ch == L'-' || ch == L':';
                    });
        }

        bool IsLowerSha256(std::wstring const& value)
        {
            return
                value.size() == 64 &&
                std::all_of(
                    value.begin(),
                    value.end(),
                    [](wchar_t ch)
                    {
                        return
                            (ch >= L'0' && ch <= L'9') ||
                            (ch >= L'a' && ch <= L'f');
                    });
        }

        std::wstring ReadString(
            JsonObject const& request,
            wchar_t const* name,
            size_t maximumLength)
        {
            if (!request.HasKey(name) ||
                request.GetNamedValue(name).ValueType() !=
                    JsonValueType::String)
            {
                FailBrokerRequest(
                    L"INVALID_ARGUMENT_TYPE",
                    std::wstring(L"field must be a string: ") + name);
            }
            auto value = std::wstring(
                request.GetNamedString(name).c_str());
            if (value.empty() || value.size() > maximumLength)
            {
                FailBrokerRequest(
                    L"INVALID_ARGUMENT_RANGE",
                    std::wstring(L"field is outside its string bound: ") +
                        name);
            }
            return value;
        }

        JsonArray TransitionsJson(
            std::vector<std::wstring> const& transitions)
        {
            JsonArray values;
            for (auto const& transition : transitions)
            {
                values.Append(
                    JsonValue::CreateStringValue(
                        hstring(transition)));
            }
            return values;
        }
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::PolicyJson() const
    {
        auto const& policy = ledger_.Policy();
        JsonObject result;
        PutBrokerString(
            result,
            L"policy_version",
            XComputeShared::XcpCoordinatorPolicyVersion);
        PutBrokerString(
            result,
            L"profile_status",
            L"CONFIGURED_BOUNDED_CANDIDATE_NOT_PLATFORM_CAPACITY");
        PutBrokerNumber(
            result,
            L"global_configured_bytes",
            policy.globalConfiguredBytes);
        PutBrokerNumber(
            result,
            L"safety_reserve_bytes",
            policy.safetyReserveBytes);
        PutBrokerNumber(
            result,
            L"per_job_bytes",
            policy.perJobBytes);
        PutBrokerNumber(
            result,
            L"metadata_journal_allowance_bytes",
            policy.metadataJournalAllowanceBytes);
        PutBrokerNumber(
            result,
            L"logical_page_bytes",
            policy.logicalPageBytes);
        PutBrokerNumber(
            result,
            L"maximum_active_reservations",
            policy.maximumActiveReservations);
        PutBrokerNumber(
            result,
            L"maximum_retained_records",
            policy.maximumRetainedRecords);
        PutBrokerNumber(
            result,
            L"maximum_simultaneous_buffers",
            policy.maximumSimultaneousBuffers);
        PutBrokerString(
            result,
            L"numeric_watermarks_status",
            L"UNKNOWN_PENDING_MEASUREMENT");
        return result;
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::Describe() const
    {
        JsonObject result;
        PutBrokerString(
            result,
            L"schema_version",
            L"xcp-persistent-compute-coordinator-description-v1");
        PutBrokerString(
            result,
            L"delivery_train",
            XComputeShared::XcpCoordinatorTrainId);
        PutBrokerString(
            result,
            L"selected_topology",
            L"CASE_B_OUT_OF_PROCESS_NARROW_COORDINATOR");
        PutBrokerString(
            result,
            L"component_role",
            L"watchdog_lifecycle_and_reservation_coordinator");
        PutBrokerBoolean(
            result,
            L"single_process_adapter_available",
            true);
        PutBrokerBoolean(
            result,
            L"semantic_authority",
            false);
        PutBrokerBoolean(
            result,
            L"artifact_publication_authority",
            false);
        PutBrokerBoolean(
            result,
            L"page_store_materialized",
            false);
        PutBrokerBoolean(
            result,
            L"persistent_journal_materialized",
            false);
        PutBrokerBoolean(
            result,
            L"second_job_queue",
            false);
        PutBrokerBoolean(
            result,
            L"second_artifact_system",
            false);
        result.SetNamedValue(L"policy", PolicyJson());
        PutBrokerNumber(
            result,
            L"active_reservation_bytes",
            ledger_.ActiveReservationBytes());
        PutBrokerNumber(
            result,
            L"active_reservation_count",
            ledger_.ActiveReservationCount());
        return result;
    }

    XComputeShared::XcpReservationBinding
        WorkerPersistentComputeCoordinatorRuntime::ReadBinding(
            JsonObject const& request) const
    {
        auto schema = ReadString(
            request,
            L"reservation_schema_version",
            96);
        if (schema != XComputeShared::XcpReservationLeaseSchema)
        {
            FailBrokerRequest(
                L"UNSUPPORTED_RESERVATION_SCHEMA",
                L"reservation_schema_version is unsupported");
        }

        XComputeShared::XcpReservationBinding binding;
        binding.jobId = ReadString(request, L"job_id", 64);
        binding.executionId = ReadString(
            request,
            L"execution_id",
            64);
        binding.storagePlanSha256 = ReadString(
            request,
            L"storage_plan_sha256",
            64);
        binding.declaredPeakBytes = ReadBrokerIntegral(
            request,
            L"declared_peak_bytes",
            0,
            true);
        binding.generation = ReadBrokerIntegral(
            request,
            L"generation",
            0,
            true);
        binding.ownerIdentity = ReadString(
            request,
            L"owner_identity",
            96);
        if (!IsSafeToken(binding.jobId, 64) ||
            !IsSafeToken(binding.executionId, 64) ||
            !IsSafeToken(binding.ownerIdentity, 96) ||
            !IsLowerSha256(binding.storagePlanSha256) ||
            binding.generation == 0)
        {
            FailBrokerRequest(
                L"INVALID_RESERVATION_BINDING",
                L"reservation identity, generation, or plan digest is invalid");
        }
        auto& domains = binding.quotaDomains;
        domains.protectedRecoveryPinnedBytes =
            ReadBrokerIntegral(
                request,
                L"protected_recovery_pinned_bytes",
                0,
                true);
        domains.evictableCacheEphemeralBytes =
            ReadBrokerIntegral(
                request,
                L"evictable_cache_ephemeral_bytes",
                0,
                true);
        domains.persistentArtifactBytes =
            ReadBrokerIntegral(
                request,
                L"persistent_artifact_bytes",
                0,
                true);
        domains.metadataJournalBytes =
            ReadBrokerIntegral(
                request,
                L"metadata_journal_bytes",
                0,
                true);
        domains.temporaryBytes = ReadBrokerIntegral(
            request,
            L"temporary_bytes",
            0,
            true);
        domains.declaredPeakBytes =
            binding.declaredPeakBytes;
        domains.declaredPhysicalGrowthBytes =
            ReadBrokerIntegral(
                request,
                L"declared_physical_growth_bytes",
                0,
                true);
        domains.platformAvailableKnown =
            ReadBrokerBoolean(
                request,
                L"platform_available_known",
                false);
        domains.platformAvailableBytes =
            ReadBrokerIntegral(
                request,
                L"platform_available_bytes",
                0,
                true);
        domains.maximumSimultaneousBuffers =
            ReadBrokerIntegral(
                request,
                L"maximum_simultaneous_buffers",
                0,
                true);
        return binding;
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::OperationJson(
            XComputeShared::XcpReservationOperation const& operation) const
    {
        JsonObject result;
        PutBrokerString(
            result,
            L"schema_version",
            XComputeShared::XcpReservationLeaseSchema);
        PutBrokerBoolean(
            result,
            L"succeeded",
            operation.succeeded);
        PutBrokerBoolean(
            result,
            L"idempotent",
            operation.idempotent);
        PutBrokerString(result, L"code", operation.code);
        PutBrokerNumber(
            result,
            L"active_reservation_bytes",
            operation.activeReservationBytes);
        PutBrokerNumber(
            result,
            L"active_reservation_count",
            operation.activeReservationCount);

        JsonObject admission;
        PutBrokerBoolean(
            admission,
            L"admitted",
            operation.admission.admitted);
        PutBrokerString(
            admission,
            L"code",
            operation.admission.code);
        PutBrokerNumber(
            admission,
            L"protected_existing_bytes",
            operation.admission.protectedExistingBytes);
        PutBrokerNumber(
            admission,
            L"required_against_configured_quota_bytes",
            operation.admission.requiredAgainstConfiguredQuotaBytes);
        PutBrokerNumber(
            admission,
            L"configured_usable_bytes",
            operation.admission.configuredUsableBytes);
        PutBrokerNumber(
            admission,
            L"required_against_platform_available_bytes",
            operation.admission.requiredAgainstPlatformAvailableBytes);
        result.SetNamedValue(L"admission", admission);

        if (!operation.reservation.reservationId.empty())
        {
            auto const& record = operation.reservation;
            JsonObject reservation;
            PutBrokerString(
                reservation,
                L"reservation_id",
                record.reservationId);
            PutBrokerString(
                reservation,
                L"job_id",
                record.binding.jobId);
            PutBrokerString(
                reservation,
                L"execution_id",
                record.binding.executionId);
            PutBrokerString(
                reservation,
                L"storage_plan_sha256",
                record.binding.storagePlanSha256);
            PutBrokerNumber(
                reservation,
                L"declared_peak_bytes",
                record.binding.declaredPeakBytes);
            PutBrokerNumber(
                reservation,
                L"generation",
                record.binding.generation);
            PutBrokerString(
                reservation,
                L"owner_identity",
                record.binding.ownerIdentity);
            PutBrokerNumber(
                reservation,
                L"creation_sequence",
                record.creationSequence);
            PutBrokerString(
                reservation,
                L"state",
                record.state);
            PutBrokerString(
                reservation,
                L"terminal_disposition",
                record.terminalDisposition);
            reservation.SetNamedValue(
                L"transitions",
                TransitionsJson(record.transitions));
            result.SetNamedValue(L"reservation", reservation);
        }
        return result;
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::Acquire(
            JsonObject const& request)
    {
        auto operation = ledger_.Acquire(ReadBinding(request));
        if (!operation.succeeded)
        {
            FailBrokerRequest(
                operation.code,
                L"reservation acquisition failed closed");
        }
        return OperationJson(operation);
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::Status(
            JsonObject const& request) const
    {
        auto reservationId = ReadString(
            request,
            L"reservation_id",
            96);
        auto operation = ledger_.Status(reservationId);
        if (!operation.succeeded)
        {
            FailBrokerRequest(
                operation.code,
                L"reservation status failed closed");
        }
        return OperationJson(operation);
    }

    JsonObject
        WorkerPersistentComputeCoordinatorRuntime::Release(
            JsonObject const& request)
    {
        auto reservationId = ReadString(
            request,
            L"reservation_id",
            96);
        auto disposition = ReadString(
            request,
            L"terminal_disposition",
            48);
        if (!IsSafeToken(disposition, 48))
        {
            FailBrokerRequest(
                L"INVALID_TERMINAL_DISPOSITION",
                L"terminal_disposition must be a bounded non-secret token");
        }
        auto operation = ledger_.Release(
            reservationId,
            ReadBinding(request),
            disposition);
        if (!operation.succeeded)
        {
            FailBrokerRequest(
                operation.code,
                L"reservation release failed closed");
        }
        return OperationJson(operation);
    }

    void WorkerPersistentComputeCoordinatorRuntime::ReleaseAll(
        std::wstring_view disposition) noexcept
    {
        ledger_.ReleaseAll(disposition);
    }
}
