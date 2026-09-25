#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerPersistentStorageBinding
    {
        std::wstring namespaceId;
        std::wstring jobId;
        std::wstring executionId;
        std::wstring datasetId;
        std::wstring executionPlanSha256;
        std::wstring storagePlanSha256;
        std::wstring reservationId;
        uint64_t generation = 0;
        uint64_t maximumBytes = 0;
        uint64_t logicalPageBytes = 4096;
        uint64_t maximumPageCount = 63;
        uint64_t maximumJournalRecords = 64;
    };

    struct WorkerPersistentPageCommit
    {
        uint64_t sequence = 0;
        std::wstring recordSha256;
        std::wstring contentSha256;
        bool deduplicated = false;
        bool logicalReplay = false;
    };

    struct WorkerPersistentRootCommit
    {
        uint64_t sequence = 0;
        std::wstring recordSha256;
        std::wstring rootSha256;
        std::wstring manifestSha256;
    };

    struct WorkerPersistentRecoverySnapshot
    {
        uint64_t validPrefixLength = 0;
        uint64_t committedPageCount = 0;
        uint64_t committedBytes = 0;
        uint64_t firstUncommittedPageIndex = 0;
        std::wstring chainHeadSha256;
        std::wstring committedRootSha256;
        std::wstring stopCode;
        std::vector<std::wstring> orderedPageSha256;
        std::vector<uint64_t> orderedPageIndices;
        bool invalidTailQuarantined = false;
        bool publicationEligible = false;
        bool recoveryLockObserved = false;
    };

    struct WorkerPersistentAppendCursor
    {
        uint64_t nextPageIndex = 0;
        uint64_t nextJournalSequence = 1;
        uint64_t committedUniqueBytes = 0;
        std::wstring previousRecordSha256;
    };

    class WorkerContentAddressedStoreRecovery final
    {
    public:
        winrt::Windows::Data::Json::JsonObject RunCoreMatrix(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding) const;

        winrt::Windows::Data::Json::JsonObject SeedRestartFixture(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding) const;

        winrt::Windows::Data::Json::JsonObject RecoverRestartFixture(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding) const;

        winrt::Windows::Data::Json::JsonObject CleanupNamespace(
            std::filesystem::path const& workerRoot,
            std::wstring const& namespaceId) const;

        WorkerPersistentRecoverySnapshot RecoverCommittedPrefix(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding) const;

        WorkerPersistentPageCommit CommitVerifiedPage(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            uint64_t pageIndex,
            std::vector<uint8_t> const& bytes,
            std::wstring const& retentionClass) const;

        WorkerPersistentAppendCursor OpenVerifiedAppendCursor(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding) const;

        WorkerPersistentPageCommit CommitVerifiedPage(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            uint64_t pageIndex,
            std::vector<uint8_t> const& bytes,
            std::wstring const& retentionClass,
            WorkerPersistentAppendCursor& cursor) const;

        WorkerPersistentRootCommit CommitVerifiedRoot(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            std::vector<std::wstring> const& orderedPageSha256) const;

        std::vector<uint8_t> ReadVerifiedCommittedPage(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            uint64_t pageIndex,
            std::wstring const& expectedSha256) const;
    };

    std::wstring WorkerContentAddressedStoreRecoveryCapabilityJson();
}
