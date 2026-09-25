#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace XComputeShared
{
    inline constexpr wchar_t const* XcpCoordinatorTrainId =
        L"PERSISTENT_COMPUTE_COORDINATOR_AND_RESERVATION_V1";
    inline constexpr wchar_t const* XcpCoordinatorPolicyVersion =
        L"xcp-storage-reservation-policy-v2";
    inline constexpr wchar_t const* XcpReservationLeaseSchema =
        L"xcp-storage-reservation-lease-v1";

    struct XcpStorageQuotaPolicy final
    {
        uint64_t globalConfiguredBytes = 81ull * 1024ull * 1024ull * 1024ull;
        uint64_t safetyReserveBytes = 16ull * 1024ull * 1024ull * 1024ull;
        uint64_t perJobBytes = 64ull * 1024ull * 1024ull * 1024ull;
        uint64_t metadataJournalAllowanceBytes = 512ull * 1024ull * 1024ull;
        uint64_t logicalPageBytes = 4ull * 1024ull * 1024ull;
        uint64_t maximumActiveReservations = 8;
        uint64_t maximumRetainedRecords = 64;
        uint64_t maximumSimultaneousBuffers = 8;
    };

    inline XcpStorageQuotaPolicy XcpDefaultStorageQuotaPolicy() noexcept
    {
        return {};
    }

    inline uint64_t XcpSaturatingAdd(
        uint64_t left,
        uint64_t right) noexcept
    {
        auto maximum = (std::numeric_limits<uint64_t>::max)();
        return right > maximum - left ? maximum : left + right;
    }

    struct XcpStorageQuotaDomains final
    {
        uint64_t protectedRecoveryPinnedBytes = 0;
        uint64_t evictableCacheEphemeralBytes = 0;
        uint64_t persistentArtifactBytes = 0;
        uint64_t metadataJournalBytes = 0;
        uint64_t temporaryBytes = 0;
        uint64_t declaredPeakBytes = 0;
        uint64_t declaredPhysicalGrowthBytes = 0;
        bool platformAvailableKnown = false;
        uint64_t platformAvailableBytes = 0;
        uint64_t maximumSimultaneousBuffers = 0;
    };

    struct XcpStorageAdmissionDecision final
    {
        bool admitted = false;
        std::wstring code = L"STORAGE_ADMISSION_NOT_EVALUATED";
        uint64_t protectedExistingBytes = 0;
        uint64_t activeReservationBytes = 0;
        uint64_t requiredAgainstConfiguredQuotaBytes = 0;
        uint64_t configuredUsableBytes = 0;
        uint64_t requiredAgainstPlatformAvailableBytes = 0;
    };

    inline XcpStorageAdmissionDecision XcpEvaluateStorageAdmission(
        XcpStorageQuotaPolicy const& policy,
        XcpStorageQuotaDomains const& domains,
        uint64_t activeReservationBytes) noexcept
    {
        XcpStorageAdmissionDecision decision;
        decision.activeReservationBytes = activeReservationBytes;
        decision.configuredUsableBytes =
            policy.globalConfiguredBytes > policy.safetyReserveBytes
                ? policy.globalConfiguredBytes - policy.safetyReserveBytes
                : 0;

        auto protectedBytes = XcpSaturatingAdd(
            domains.protectedRecoveryPinnedBytes,
            domains.persistentArtifactBytes);
        protectedBytes = XcpSaturatingAdd(
            protectedBytes,
            domains.metadataJournalBytes);
        protectedBytes = XcpSaturatingAdd(
            protectedBytes,
            domains.temporaryBytes);
        decision.protectedExistingBytes = protectedBytes;

        auto required = XcpSaturatingAdd(
            protectedBytes,
            activeReservationBytes);
        required = XcpSaturatingAdd(
            required,
            domains.declaredPeakBytes);
        required = XcpSaturatingAdd(
            required,
            policy.metadataJournalAllowanceBytes);
        decision.requiredAgainstConfiguredQuotaBytes = required;
        decision.requiredAgainstPlatformAvailableBytes =
            XcpSaturatingAdd(
                domains.declaredPhysicalGrowthBytes,
                policy.safetyReserveBytes);

        if (domains.declaredPeakBytes == 0)
        {
            decision.code = L"DECLARED_PEAK_REQUIRED";
            return decision;
        }
        if (domains.declaredPeakBytes > policy.perJobBytes)
        {
            decision.code = L"PER_JOB_QUOTA_EXCEEDED";
            return decision;
        }
        if (domains.metadataJournalBytes >
            policy.metadataJournalAllowanceBytes)
        {
            decision.code = L"METADATA_JOURNAL_ALLOWANCE_EXCEEDED";
            return decision;
        }
        if (domains.temporaryBytes > domains.declaredPeakBytes ||
            domains.declaredPhysicalGrowthBytes >
                domains.declaredPeakBytes)
        {
            decision.code = L"DECLARED_PEAK_INCONSISTENT";
            return decision;
        }
        if (domains.maximumSimultaneousBuffers == 0 ||
            domains.maximumSimultaneousBuffers >
                policy.maximumSimultaneousBuffers)
        {
            decision.code = L"SIMULTANEOUS_BUFFER_BOUND_INVALID";
            return decision;
        }
        if (required > decision.configuredUsableBytes)
        {
            decision.code = L"GLOBAL_QUOTA_MINUS_RESERVE_EXCEEDED";
            return decision;
        }
        if (domains.platformAvailableKnown &&
            decision.requiredAgainstPlatformAvailableBytes >
                domains.platformAvailableBytes)
        {
            decision.code = L"PLATFORM_AVAILABLE_MINUS_RESERVE_EXCEEDED";
            return decision;
        }

        decision.admitted = true;
        decision.code = L"ADMITTED";
        return decision;
    }

    struct XcpReservationBinding final
    {
        std::wstring jobId;
        std::wstring executionId;
        std::wstring storagePlanSha256;
        uint64_t declaredPeakBytes = 0;
        uint64_t generation = 0;
        std::wstring ownerIdentity;
        XcpStorageQuotaDomains quotaDomains;
    };

    struct XcpReservationSnapshot final
    {
        std::wstring reservationId;
        XcpReservationBinding binding;
        uint64_t creationSequence = 0;
        std::wstring state = L"requested";
        std::wstring terminalDisposition = L"none";
        std::vector<std::wstring> transitions;
    };

    struct XcpReservationOperation final
    {
        bool succeeded = false;
        bool idempotent = false;
        std::wstring code = L"RESERVATION_NOT_EVALUATED";
        XcpReservationSnapshot reservation;
        XcpStorageAdmissionDecision admission;
        uint64_t activeReservationBytes = 0;
        uint64_t activeReservationCount = 0;
    };

    inline bool XcpReservationBindingsEqual(
        XcpReservationBinding const& left,
        XcpReservationBinding const& right) noexcept
    {
        return
            left.jobId == right.jobId &&
            left.executionId == right.executionId &&
            left.storagePlanSha256 == right.storagePlanSha256 &&
            left.declaredPeakBytes == right.declaredPeakBytes &&
            left.generation == right.generation &&
            left.ownerIdentity == right.ownerIdentity;
    }

    class XcpStorageReservationLedger final
    {
    public:
        explicit XcpStorageReservationLedger(
            XcpStorageQuotaPolicy policy =
                XcpDefaultStorageQuotaPolicy())
            : policy_(policy)
        {
        }

        XcpStorageQuotaPolicy const& Policy() const noexcept
        {
            return policy_;
        }

        uint64_t ActiveReservationBytes() const noexcept
        {
            return activeReservationBytes_;
        }

        uint64_t ActiveReservationCount() const noexcept
        {
            return activeReservationCount_;
        }

        XcpReservationOperation Acquire(
            XcpReservationBinding const& binding)
        {
            XcpReservationOperation operation;
            for (auto const& [reservationId, record] : records_)
            {
                (void)reservationId;
                if (record.binding.jobId != binding.jobId ||
                    record.binding.executionId != binding.executionId)
                {
                    continue;
                }
                if (record.state == L"active" &&
                    XcpReservationBindingsEqual(
                        record.binding,
                        binding))
                {
                    operation.succeeded = true;
                    operation.idempotent = true;
                    operation.code = L"RESERVATION_ALREADY_ACTIVE";
                    operation.reservation = record;
                    AttachTotals(operation);
                    return operation;
                }
                if (record.state == L"active")
                {
                    operation.code =
                        L"ACTIVE_RESERVATION_BINDING_CONFLICT";
                    AttachTotals(operation);
                    return operation;
                }
                if (XcpReservationBindingsEqual(
                        record.binding,
                        binding))
                {
                    operation.code =
                        L"TERMINAL_RESERVATION_CANNOT_REACTIVATE";
                    operation.reservation = record;
                    AttachTotals(operation);
                    return operation;
                }
            }
            operation.admission = XcpEvaluateStorageAdmission(
                policy_,
                binding.quotaDomains,
                activeReservationBytes_);
            if (!operation.admission.admitted)
            {
                operation.code = operation.admission.code;
                AttachTotals(operation);
                return operation;
            }
            if (activeReservationCount_ >=
                policy_.maximumActiveReservations)
            {
                operation.code =
                    L"ACTIVE_RESERVATION_COUNT_EXCEEDED";
                AttachTotals(operation);
                return operation;
            }
            if (records_.size() >= policy_.maximumRetainedRecords)
            {
                operation.code =
                    L"RESERVATION_RECORD_BOUND_EXCEEDED";
                AttachTotals(operation);
                return operation;
            }

            XcpReservationSnapshot record;
            record.binding = binding;
            record.creationSequence = ++creationSequence_;
            record.reservationId =
                L"xcp-lease-" +
                std::to_wstring(record.creationSequence) +
                L"-" +
                binding.storagePlanSha256.substr(
                    0,
                    (std::min)(size_t{ 12 },
                        binding.storagePlanSha256.size()));
            record.transitions = { L"requested", L"active" };
            record.state = L"active";
            records_.emplace(record.reservationId, record);
            activeReservationBytes_ = XcpSaturatingAdd(
                activeReservationBytes_,
                binding.declaredPeakBytes);
            ++activeReservationCount_;

            operation.succeeded = true;
            operation.code = L"RESERVATION_ACQUIRED";
            operation.reservation = record;
            AttachTotals(operation);
            return operation;
        }

        XcpReservationOperation Status(
            std::wstring_view reservationId) const
        {
            XcpReservationOperation operation;
            auto found = records_.find(std::wstring(reservationId));
            if (found == records_.end())
            {
                operation.code = L"RESERVATION_NOT_FOUND";
                AttachTotals(operation);
                return operation;
            }
            operation.succeeded = true;
            operation.code = L"RESERVATION_FOUND";
            operation.reservation = found->second;
            AttachTotals(operation);
            return operation;
        }

        XcpReservationOperation Release(
            std::wstring_view reservationId,
            XcpReservationBinding const& expectedBinding,
            std::wstring_view terminalDisposition)
        {
            XcpReservationOperation operation;
            auto found = records_.find(std::wstring(reservationId));
            if (found == records_.end())
            {
                operation.code = L"RESERVATION_NOT_FOUND";
                AttachTotals(operation);
                return operation;
            }
            if (!XcpReservationBindingsEqual(
                    found->second.binding,
                    expectedBinding))
            {
                operation.code =
                    L"RESERVATION_IDENTITY_MISMATCH";
                AttachTotals(operation);
                return operation;
            }
            if (found->second.state == L"released")
            {
                operation.succeeded = true;
                operation.idempotent = true;
                operation.code = L"RESERVATION_ALREADY_RELEASED";
                operation.reservation = found->second;
                AttachTotals(operation);
                return operation;
            }

            found->second.transitions.push_back(L"releasing");
            found->second.state = L"releasing";
            found->second.transitions.push_back(L"released");
            found->second.state = L"released";
            found->second.terminalDisposition =
                std::wstring(terminalDisposition);
            activeReservationBytes_ =
                found->second.binding.declaredPeakBytes >
                    activeReservationBytes_
                    ? 0
                    : activeReservationBytes_ -
                        found->second.binding.declaredPeakBytes;
            if (activeReservationCount_ != 0)
            {
                --activeReservationCount_;
            }

            operation.succeeded = true;
            operation.code = L"RESERVATION_RELEASED";
            operation.reservation = found->second;
            AttachTotals(operation);
            return operation;
        }

        void ReleaseAll(std::wstring_view terminalDisposition) noexcept
        {
            for (auto& [reservationId, record] : records_)
            {
                (void)reservationId;
                if (record.state != L"active")
                {
                    continue;
                }
                record.transitions.push_back(L"releasing");
                record.state = L"releasing";
                record.transitions.push_back(L"released");
                record.state = L"released";
                record.terminalDisposition =
                    std::wstring(terminalDisposition);
            }
            activeReservationBytes_ = 0;
            activeReservationCount_ = 0;
        }

    private:
        void AttachTotals(
            XcpReservationOperation& operation) const noexcept
        {
            operation.activeReservationBytes =
                activeReservationBytes_;
            operation.activeReservationCount =
                activeReservationCount_;
        }

        XcpStorageQuotaPolicy policy_;
        std::map<std::wstring, XcpReservationSnapshot> records_;
        uint64_t creationSequence_ = 0;
        uint64_t activeReservationBytes_ = 0;
        uint64_t activeReservationCount_ = 0;
    };
}
