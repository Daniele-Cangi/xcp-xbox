#include "pch.h"
#include "WorkerStorageScaleCharacterizationRuntime.h"
#include "WorkerProcessTopologyProtocol.h"

#include <array>
#include <cstring>
#include <set>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        using namespace ProcessTopology;
        namespace fs = std::filesystem;

        constexpr uint64_t KiB = 1024ull;
        constexpr uint64_t MiB = 1024ull * KiB;
        constexpr uint64_t GiB = 1024ull * MiB;
        constexpr uint64_t PageBytes = 4ull * MiB;
        constexpr uint64_t TargetBytes = 8ull * GiB;
        constexpr uint64_t SeedBytes = 8ull * GiB;
        constexpr uint64_t SafetyReserveBytes = 16ull * GiB;
        constexpr uint64_t TargetPageCount =
            TargetBytes / PageBytes;
        constexpr uint64_t SeedPageCount =
            SeedBytes / PageBytes;
        constexpr uint64_t MaximumJournalRecords =
            TargetPageCount + 1;
        constexpr uint64_t MaximumSimultaneousBuffers = 2;
        constexpr std::array<uint64_t, 6> CapacityLadder{
            256ull * MiB,
            512ull * MiB,
            1ull * GiB,
            2ull * GiB,
            4ull * GiB,
            8ull * GiB
        };

        double ElapsedMilliseconds(
            std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }

        void PutMetric(
            JsonObject const& value,
            wchar_t const* name,
            double number)
        {
            value.SetNamedValue(
                name,
                JsonValue::CreateNumberValue(number));
        }

        uint64_t SaturatingAdd(
            uint64_t left,
            uint64_t right)
        {
            auto maximum =
                (std::numeric_limits<uint64_t>::max)();
            return right > maximum - left
                ? maximum
                : left + right;
        }

        JsonObject ObserveSpace(
            fs::path const& workerRoot,
            uint64_t logicalCommittedBytes)
        {
            std::error_code error;
            auto info = fs::space(
                workerRoot.parent_path(),
                error);
            if (error)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_SPACE_OBSERVATION_FAILED",
                    "platform storage capacity could not be observed");
            }
            JsonObject value;
            PutNumber(
                value,
                L"logical_committed_bytes",
                logicalCommittedBytes);
            PutNumber(
                value,
                L"capacity_bytes",
                info.capacity);
            PutNumber(
                value,
                L"free_bytes",
                info.free);
            PutNumber(
                value,
                L"available_bytes",
                info.available);
            return value;
        }

        void RequirePhysicalHeadroom(
            fs::path const& workerRoot,
            uint64_t declaredGrowthBytes)
        {
            auto observation =
                ObserveSpace(workerRoot, 0);
            auto available = static_cast<uint64_t>(
                observation.GetNamedNumber(
                    L"available_bytes"));
            auto required = SaturatingAdd(
                declaredGrowthBytes,
                SafetyReserveBytes);
            if (available < required)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_PLATFORM_HEADROOM_REFUSED",
                    "available storage does not cover declared growth plus the fixed 16 GiB reserve");
            }
        }

        void BuildDeterministicPage(
            uint64_t pageIndex,
            std::vector<uint8_t>& bytes)
        {
            constexpr size_t PatternBytes = 4096;
            bytes.resize(static_cast<size_t>(PageBytes));
            std::array<uint8_t, PatternBytes> pattern{};
            auto state =
                0x9e3779b97f4a7c15ull ^
                (pageIndex + 1) *
                    0xbf58476d1ce4e5b9ull;
            for (size_t offset = 0;
                 offset < PatternBytes;
                 offset += sizeof(uint64_t))
            {
                state ^= state >> 12;
                state ^= state << 25;
                state ^= state >> 27;
                auto value =
                    state * 0x2545f4914f6cdd1dull;
                std::memcpy(
                    pattern.data() + offset,
                    &value,
                    sizeof(value));
            }
            std::memcpy(
                pattern.data(),
                &pageIndex,
                sizeof(pageIndex));
            for (size_t offset = 0;
                 offset < bytes.size();
                 offset += PatternBytes)
            {
                std::memcpy(
                    bytes.data() + offset,
                    pattern.data(),
                    PatternBytes);
            }
        }

        void ValidateGeometry(
            WorkerPersistentStorageBinding const& binding)
        {
            if (binding.logicalPageBytes != PageBytes ||
                binding.maximumPageCount != TargetPageCount ||
                binding.maximumJournalRecords !=
                    MaximumJournalRecords ||
                binding.maximumBytes != TargetBytes)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_GEOMETRY_INVALID",
                    "the persistent binding differs from the frozen 8 GiB geometry");
            }
        }

        void ValidateRestartAuthorization(
            WorkerPersistentStorageBinding const& binding,
            WorkerStorageScaleRestartAuthorization const*
                authorization)
        {
            if (authorization == nullptr ||
                authorization->persistedReservationId !=
                    binding.reservationId ||
                authorization->persistedGeneration !=
                    binding.generation ||
                authorization->activeReservationId.empty() ||
                authorization->activeGeneration <=
                    authorization->persistedGeneration ||
                !authorization->workerBrokerBindingAgreed)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_REAUTHORIZATION_INVALID",
                    "restart operations require a fresh lease over the exact persisted binding");
            }
        }

        void ValidateOrderedUniquePrefix(
            WorkerPersistentRecoverySnapshot const& recovered,
            uint64_t expectedPageCount,
            bool expectRoot)
        {
            if (recovered.invalidTailQuarantined ||
                recovered.committedPageCount !=
                    expectedPageCount ||
                recovered.committedBytes !=
                    expectedPageCount * PageBytes ||
                recovered.orderedPageSha256.size() !=
                    expectedPageCount ||
                recovered.orderedPageIndices.size() !=
                    expectedPageCount ||
                recovered.publicationEligible != expectRoot ||
                expectRoot !=
                    !recovered.committedRootSha256.empty())
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_RECOVERY_PREFIX_INVALID",
                    "the recovered storage prefix differs from the expected scale checkpoint");
            }
            std::set<std::wstring> uniqueHashes;
            for (uint64_t index = 0;
                 index < expectedPageCount;
                 ++index)
            {
                if (recovered.orderedPageIndices[index] !=
                        index ||
                    !uniqueHashes.insert(
                        recovered.orderedPageSha256[index]).second)
                {
                    throw WorkerStorageScaleCharacterizationError(
                        L"STORAGE_SCALE_RECOVERY_PREFIX_INVALID",
                        "the recovered scale pages are not contiguous and content-unique");
                }
            }
        }

        JsonObject RecoverySummary(
            WorkerPersistentRecoverySnapshot const& recovered)
        {
            JsonObject value;
            PutNumber(
                value,
                L"valid_prefix_length",
                recovered.validPrefixLength);
            PutNumber(
                value,
                L"committed_page_count",
                recovered.committedPageCount);
            PutNumber(
                value,
                L"committed_bytes",
                recovered.committedBytes);
            PutNumber(
                value,
                L"first_uncommitted_page_index",
                recovered.firstUncommittedPageIndex);
            PutString(
                value,
                L"chain_head_sha256",
                recovered.chainHeadSha256);
            PutString(
                value,
                L"committed_root_sha256",
                recovered.committedRootSha256);
            PutString(
                value,
                L"stop_code",
                recovered.stopCode);
            PutBool(
                value,
                L"invalid_tail_quarantined",
                recovered.invalidTailQuarantined);
            PutBool(
                value,
                L"publication_eligible",
                recovered.publicationEligible);
            return value;
        }
    }

    WorkerStorageScaleCharacterizationError::
        WorkerStorageScaleCharacterizationError(
            std::wstring codeValue,
            std::string messageValue)
        : code(std::move(codeValue)),
          message(std::move(messageValue))
    {
    }

    char const*
    WorkerStorageScaleCharacterizationError::what() const noexcept
    {
        return message.c_str();
    }

    JsonObject
    WorkerStorageScaleCharacterizationRuntime::Execute(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        WorkerStorageScaleRestartAuthorization const*
            restartAuthorization,
        std::wstring const& operation) const
    {
        auto started = std::chrono::steady_clock::now();
        ValidateGeometry(binding);
        if (operation != L"seed" &&
            operation != L"resume" &&
            operation != L"verify" &&
            operation != L"cleanup")
        {
            throw WorkerStorageScaleCharacterizationError(
                L"STORAGE_SCALE_OPERATION_INVALID",
                "the storage scale operation is unsupported");
        }
        if (operation == L"seed")
        {
            if (restartAuthorization != nullptr)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_REAUTHORIZATION_UNEXPECTED",
                    "seed cannot carry restart authorization");
            }
        }
        else
        {
            ValidateRestartAuthorization(
                binding,
                restartAuthorization);
        }

        auto spaceBefore = ObserveSpace(workerRoot, 0);
        WorkerContentAddressedStoreRecovery storage;
        JsonObject result;
        PutString(
            result,
            L"schema_version",
            L"xcp-storage-scale-characterization-v1-operation-result");
        PutString(result, L"operation", operation);
        PutNumber(
            result,
            L"target_bytes",
            TargetBytes);
        PutNumber(
            result,
            L"logical_page_bytes",
            PageBytes);
        PutNumber(
            result,
            L"target_page_count",
            TargetPageCount);
        PutNumber(
            result,
            L"safety_reserve_bytes",
            SafetyReserveBytes);
        result.SetNamedValue(
            L"space_before",
            spaceBefore);

        if (operation == L"cleanup")
        {
            auto cleanup = storage.CleanupNamespace(
                workerRoot,
                binding.namespaceId);
            PutBool(
                result,
                L"ok",
                cleanup.GetNamedBoolean(
                    L"namespace_absent"));
            result.SetNamedValue(
                L"cleanup",
                cleanup);
            result.SetNamedValue(
                L"space_after",
                ObserveSpace(workerRoot, 0));
            PutMetric(
                result,
                L"elapsed_ms",
                ElapsedMilliseconds(started));
            return result;
        }

        auto recoveryStarted =
            std::chrono::steady_clock::now();
        auto recovered =
            storage.RecoverCommittedPrefix(
                workerRoot,
                binding);
        auto recoveryMs =
            ElapsedMilliseconds(recoveryStarted);
        if (operation == L"seed")
        {
            ValidateOrderedUniquePrefix(
                recovered,
                0,
                false);
            RequirePhysicalHeadroom(
                workerRoot,
                SeedBytes);
        }
        else if (operation == L"resume")
        {
            ValidateOrderedUniquePrefix(
                recovered,
                SeedPageCount,
                false);
            RequirePhysicalHeadroom(
                workerRoot,
                TargetBytes - SeedBytes);
        }
        else
        {
            ValidateOrderedUniquePrefix(
                recovered,
                TargetPageCount,
                true);
        }

        result.SetNamedValue(
            L"recovery",
            RecoverySummary(recovered));
        PutMetric(
            result,
            L"recovery_ms",
            recoveryMs);
        PutNumber(
            result,
            L"verified_read_bytes",
            recovered.committedBytes);

        if (operation == L"verify")
        {
            PutBool(result, L"ok", true);
            PutString(
                result,
                L"outcome",
                L"FULL_8_GIB_ROOT_VERIFIED");
            result.SetNamedValue(
                L"space_after",
                ObserveSpace(
                    workerRoot,
                    TargetBytes));
            PutMetric(
                result,
                L"elapsed_ms",
                ElapsedMilliseconds(started));
            return result;
        }

        auto targetPageCount =
            operation == L"seed"
                ? SeedPageCount
                : TargetPageCount;
        auto pageHashes =
            recovered.orderedPageSha256;
        WorkerPersistentAppendCursor cursor;
        cursor.nextPageIndex =
            recovered.firstUncommittedPageIndex;
        cursor.nextJournalSequence =
            recovered.validPrefixLength + 1;
        cursor.committedUniqueBytes =
            recovered.committedBytes;
        cursor.previousRecordSha256 =
            recovered.chainHeadSha256;

        JsonArray rungObservations;
        std::vector<uint8_t> page;
        auto appendStarted =
            std::chrono::steady_clock::now();
        for (uint64_t pageIndex =
                 cursor.nextPageIndex;
             pageIndex < targetPageCount;
             ++pageIndex)
        {
            BuildDeterministicPage(pageIndex, page);
            auto committed =
                storage.CommitVerifiedPage(
                    workerRoot,
                    binding,
                    pageIndex,
                    page,
                    L"RECOVERY",
                    cursor);
            if (committed.logicalReplay)
            {
                throw WorkerStorageScaleCharacterizationError(
                    L"STORAGE_SCALE_LOGICAL_REPLAY_UNEXPECTED",
                    "scale growth encountered an unexpected logical replay");
            }
            pageHashes.push_back(
                committed.contentSha256);
            auto logicalBytes =
                (pageIndex + 1) * PageBytes;
            if (std::find(
                    CapacityLadder.begin(),
                    CapacityLadder.end(),
                    logicalBytes) !=
                CapacityLadder.end())
            {
                rungObservations.Append(
                    ObserveSpace(
                        workerRoot,
                        logicalBytes));
            }
        }
        auto appendMs =
            ElapsedMilliseconds(appendStarted);

        std::wstring rootSha256;
        std::wstring manifestSha256;
        if (operation == L"resume")
        {
            auto root = storage.CommitVerifiedRoot(
                workerRoot,
                binding,
                pageHashes);
            rootSha256 = root.rootSha256;
            manifestSha256 = root.manifestSha256;
        }

        PutBool(result, L"ok", true);
        PutString(
            result,
            L"outcome",
            operation == L"seed"
                ? L"SEED_8_GIB_COMMITTED_FOR_RESTART"
                : L"ROOT_8_GIB_COMMITTED_FOR_VERIFICATION");
        PutNumber(
            result,
            L"committed_page_count",
            targetPageCount);
        PutNumber(
            result,
            L"committed_bytes",
            targetPageCount * PageBytes);
        PutMetric(
            result,
            L"append_ms",
            appendMs);
        PutString(
            result,
            L"committed_root_sha256",
            rootSha256);
        PutString(
            result,
            L"manifest_sha256",
            manifestSha256);
        result.SetNamedValue(
            L"rung_observations",
            rungObservations);
        result.SetNamedValue(
            L"space_after",
            ObserveSpace(
                workerRoot,
                targetPageCount * PageBytes));
        PutMetric(
            result,
            L"elapsed_ms",
            ElapsedMilliseconds(started));
        return result;
    }

    std::wstring
    WorkerStorageScaleCharacterizationCapabilityJson()
    {
        return
            L"{\"available\":true"
            L",\"status\":\"IMPLEMENTED_LOCAL_NOT_LIVE_MEASURED\""
            L",\"delivery_train\":\"STORAGE_SCALE_CHARACTERIZATION_V1\""
            L",\"request_schema\":\"xcp-storage-scale-characterization-v1-runtime-request\""
            L",\"result_schema\":\"xcp-storage-scale-characterization-v1-runtime-result\""
            L",\"operations\":[\"seed\",\"resume\",\"verify\",\"cleanup\"]"
            L",\"target_bytes\":8589934592"
            L",\"logical_page_bytes\":4194304"
            L",\"target_page_count\":2048"
            L",\"seed_bytes\":8589934592"
            L",\"safety_reserve_bytes\":17179869184"
            L",\"platform_space_observed_directly\":true"
            L",\"fresh_lease_required_after_seed\":true"
            L",\"full_root_verification_required\":true"
            L",\"live_measured\":false}";
    }
}
