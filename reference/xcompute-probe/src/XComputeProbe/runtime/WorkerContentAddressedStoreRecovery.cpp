#include "pch.h"
#include "WorkerContentAddressedStoreRecovery.h"

#include "WorkerArtifactStore.h"
#include "WorkerProcessTopologyProtocol.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    namespace
    {
        using namespace ProcessTopology;

        constexpr wchar_t const* TrainId =
            L"CONTENT_ADDRESSED_STORE_RECOVERY_V1";
        constexpr wchar_t const* PageSchema =
            L"xcp-content-addressed-page-record-v1";
        constexpr wchar_t const* JournalSchema =
            L"xcp-storage-journal-record-v1";
        constexpr wchar_t const* RootSchema =
            L"xcp-committed-page-root-v1";
        constexpr uint64_t LogicalPageBytes = 4096;
        constexpr uint64_t MaximumProbeBytes =
            64ull * 1024ull * 1024ull * 1024ull;
        constexpr uint64_t MaximumJournalRecords = 64;
        constexpr uint64_t MaximumStreamingPageBytes =
            4ull * 1024ull * 1024ull;
        constexpr uint64_t MaximumStreamingPageCount = 16384;
        constexpr uint64_t MaximumStreamingJournalRecords = 16385;

        struct StoreError : std::exception
        {
            std::wstring code;
            std::string message;

            StoreError(std::wstring codeValue, std::string messageValue)
                : code(std::move(codeValue)),
                  message(std::move(messageValue))
            {
            }

            char const* what() const noexcept override
            {
                return message.c_str();
            }
        };

        struct PageCommit
        {
            uint64_t sequence = 0;
            std::wstring recordSha256;
            std::wstring contentSha256;
            bool deduplicated = false;
            bool logicalReplay = false;
        };

        struct RecoverySnapshot
        {
            uint64_t validPrefixLength = 0;
            uint64_t committedPageCount = 0;
            uint64_t committedBytes = 0;
            uint64_t firstUncommittedPageIndex = 0;
            std::wstring chainHeadSha256;
            std::wstring committedRootSha256;
            std::wstring stopCode = L"END_OF_JOURNAL";
            bool invalidTailQuarantined = false;
            bool publicationEligible = false;
            bool recoveryLockObserved = false;
            std::vector<std::wstring> orderedPageSha256;
            std::vector<uint64_t> orderedPageIndices;
        };

        struct JournalRecord
        {
            uint64_t sequence = 0;
            std::wstring previousRecordSha256;
            std::wstring recordType;
            uint64_t pageIndex = 0;
            uint64_t logicalOffset = 0;
            uint64_t byteLength = 0;
            std::wstring contentSha256;
            std::wstring state;
            std::wstring retentionClass;
            std::wstring committedRootSha256;
            std::wstring manifestSha256;
            bool committed = true;
            std::wstring recordSha256;
        };

        std::wstring RequiredStringLocal(
            JsonObject const& value,
            wchar_t const* name)
        {
            if (!value || !value.HasKey(name) ||
                value.GetNamedValue(name).ValueType() !=
                    JsonValueType::String)
            {
                throw StoreError(
                    L"SCHEMA_FIELD_INVALID",
                    "required storage string is absent or invalid");
            }
            auto text = value.GetNamedString(name);
            return std::wstring(text.c_str());
        }

        uint64_t RequiredUInt64Local(
            JsonObject const& value,
            wchar_t const* name)
        {
            if (!value || !value.HasKey(name) ||
                value.GetNamedValue(name).ValueType() !=
                    JsonValueType::Number)
            {
                throw StoreError(
                    L"SCHEMA_FIELD_INVALID",
                    "required storage number is absent or invalid");
            }
            auto number = value.GetNamedNumber(name);
            if (number < 0 ||
                number > 9007199254740991.0 ||
                std::floor(number) != number)
            {
                throw StoreError(
                    L"SCHEMA_FIELD_INVALID",
                    "required storage number is out of range");
            }
            return static_cast<uint64_t>(number);
        }

        bool RequiredBoolLocal(
            JsonObject const& value,
            wchar_t const* name)
        {
            if (!value || !value.HasKey(name) ||
                value.GetNamedValue(name).ValueType() !=
                    JsonValueType::Boolean)
            {
                throw StoreError(
                    L"SCHEMA_FIELD_INVALID",
                    "required storage boolean is absent or invalid");
            }
            return value.GetNamedBoolean(name);
        }

        void ValidateBinding(WorkerPersistentStorageBinding const& binding)
        {
            if (!IsSafeToken(binding.namespaceId, 40) ||
                !IsSafeToken(binding.jobId, 64) ||
                !IsSafeToken(binding.executionId, 64) ||
                !IsSafeToken(binding.datasetId, 64) ||
                !IsSafeToken(binding.reservationId, 96) ||
                !IsLowerHex(binding.executionPlanSha256, 64) ||
                !IsLowerHex(binding.storagePlanSha256, 64) ||
                binding.generation == 0 ||
                binding.logicalPageBytes == 0 ||
                binding.logicalPageBytes > MaximumStreamingPageBytes ||
                binding.maximumPageCount == 0 ||
                binding.maximumPageCount > MaximumStreamingPageCount ||
                binding.maximumJournalRecords !=
                    binding.maximumPageCount + 1 ||
                binding.maximumJournalRecords >
                    MaximumStreamingJournalRecords ||
                binding.maximumBytes < binding.logicalPageBytes ||
                binding.maximumBytes > MaximumProbeBytes)
            {
                throw StoreError(
                    L"STORAGE_BINDING_INVALID",
                    "persistent storage binding is invalid");
            }
        }

        fs::path StorageRoot(fs::path const& workerRoot)
        {
            return workerRoot.parent_path() / L"xcompute-storage-v1";
        }

        fs::path NamespaceRoot(
            fs::path const& workerRoot,
            std::wstring const& namespaceId)
        {
            if (!IsSafeToken(namespaceId, 64))
            {
                throw StoreError(
                    L"NAMESPACE_INVALID",
                    "storage namespace is invalid");
            }
            return StorageRoot(workerRoot) /
                L"namespaces" /
                namespaceId;
        }

        fs::path PageRecordPath(
            fs::path const& namespaceRoot,
            uint64_t pageIndex)
        {
            return namespaceRoot /
                L"pages" /
                (std::to_wstring(pageIndex) + L".json");
        }

        fs::path JournalRecordPath(
            fs::path const& namespaceRoot,
            uint64_t sequence)
        {
            std::wostringstream name;
            name << std::setw(8) << std::setfill(L'0') <<
                sequence << L".json";
            return namespaceRoot / L"journal" / name.str();
        }

        std::string ReadText(fs::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw StoreError(
                    L"FILE_READ_FAILED",
                    "storage metadata could not be opened");
            }
            return std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }

        JsonObject ReadJson(fs::path const& path)
        {
            try
            {
                return JsonObject::Parse(
                    to_hstring(ReadText(path)));
            }
            catch (StoreError const&)
            {
                throw;
            }
            catch (...)
            {
                throw StoreError(
                    L"JOURNAL_PARSE_INVALID",
                    "storage metadata JSON is invalid");
            }
        }

        void WriteText(
            fs::path const& path,
            std::string const& content,
            bool replace)
        {
            fs::create_directories(path.parent_path());
            auto pending = path;
            pending += L".pending";
            if (fs::exists(pending))
            {
                fs::remove(pending);
            }
            {
                std::ofstream output(
                    pending,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw StoreError(
                        L"FILE_WRITE_FAILED",
                        "storage metadata staging failed");
                }
                output.write(
                    content.data(),
                    static_cast<std::streamsize>(content.size()));
                output.flush();
                if (!output.good())
                {
                    throw StoreError(
                        L"FILE_WRITE_FAILED",
                        "storage metadata write did not complete");
                }
            }
            if (fs::exists(path))
            {
                if (!replace)
                {
                    fs::remove(pending);
                    throw StoreError(
                        L"APPEND_ONLY_RECORD_EXISTS",
                        "append-only storage record already exists");
                }
                fs::remove(path);
            }
            fs::rename(pending, path);
        }

        void WriteJson(
            fs::path const& path,
            JsonObject const& value,
            bool replace = false)
        {
            WriteText(
                path,
                to_string(value.Stringify()),
                replace);
        }

        std::wstring RecordCanonical(
            WorkerPersistentStorageBinding const& binding,
            JournalRecord const& record)
        {
            std::wostringstream text;
            text <<
                JournalSchema << L"\n" <<
                record.sequence << L"\n" <<
                record.previousRecordSha256 << L"\n" <<
                record.recordType << L"\n" <<
                binding.jobId << L"\n" <<
                binding.executionId << L"\n" <<
                binding.datasetId << L"\n" <<
                binding.executionPlanSha256 << L"\n" <<
                binding.storagePlanSha256 << L"\n" <<
                binding.reservationId << L"\n" <<
                binding.generation << L"\n" <<
                record.pageIndex << L"\n" <<
                record.logicalOffset << L"\n" <<
                record.byteLength << L"\n" <<
                record.contentSha256 << L"\n" <<
                record.state << L"\n" <<
                record.retentionClass << L"\n" <<
                record.committedRootSha256 << L"\n" <<
                record.manifestSha256 << L"\n" <<
                (record.committed ? L"true" : L"false");
            return text.str();
        }

        std::wstring HashWide(std::wstring const& text)
        {
            return WorkerContentSha256(
                to_string(hstring(text)));
        }

        JsonObject RecordJson(
            WorkerPersistentStorageBinding const& binding,
            JournalRecord record)
        {
            record.recordSha256 = HashWide(
                RecordCanonical(binding, record));
            JsonObject value;
            PutString(value, L"schema_version", JournalSchema);
            PutNumber(value, L"sequence", record.sequence);
            PutString(
                value,
                L"previous_record_sha256",
                record.previousRecordSha256);
            PutString(value, L"record_type", record.recordType);
            PutString(value, L"job_id", binding.jobId);
            PutString(value, L"execution_id", binding.executionId);
            PutString(value, L"dataset_id", binding.datasetId);
            PutString(
                value,
                L"execution_plan_sha256",
                binding.executionPlanSha256);
            PutString(
                value,
                L"storage_plan_sha256",
                binding.storagePlanSha256);
            PutString(
                value,
                L"reservation_id",
                binding.reservationId);
            PutNumber(value, L"generation", binding.generation);
            PutNumber(value, L"page_index", record.pageIndex);
            PutNumber(value, L"logical_offset", record.logicalOffset);
            PutNumber(value, L"byte_length", record.byteLength);
            PutString(
                value,
                L"content_sha256",
                record.contentSha256);
            PutString(value, L"state", record.state);
            PutString(
                value,
                L"retention_class",
                record.retentionClass);
            PutString(
                value,
                L"committed_root_sha256",
                record.committedRootSha256);
            PutString(
                value,
                L"manifest_sha256",
                record.manifestSha256);
            PutBool(value, L"committed", record.committed);
            PutString(
                value,
                L"record_sha256",
                record.recordSha256);
            return value;
        }

        JournalRecord ParseRecord(
            JsonObject const& value,
            WorkerPersistentStorageBinding const& binding)
        {
            if (RequiredStringLocal(value, L"schema_version") !=
                    JournalSchema ||
                RequiredStringLocal(value, L"job_id") !=
                    binding.jobId ||
                RequiredStringLocal(value, L"execution_id") !=
                    binding.executionId ||
                RequiredStringLocal(value, L"dataset_id") !=
                    binding.datasetId ||
                RequiredStringLocal(
                    value,
                    L"execution_plan_sha256") !=
                    binding.executionPlanSha256 ||
                RequiredStringLocal(
                    value,
                    L"storage_plan_sha256") !=
                    binding.storagePlanSha256 ||
                RequiredUInt64Local(value, L"generation") !=
                    binding.generation)
            {
                throw StoreError(
                    L"PLAN_BINDING_MISMATCH",
                    "journal plan binding changed");
            }
            if (RequiredStringLocal(value, L"reservation_id") !=
                binding.reservationId)
            {
                throw StoreError(
                    L"RESERVATION_BINDING_MISMATCH",
                    "journal reservation binding changed");
            }

            JournalRecord record;
            record.sequence =
                RequiredUInt64Local(value, L"sequence");
            record.previousRecordSha256 =
                RequiredStringLocal(
                    value,
                    L"previous_record_sha256");
            record.recordType =
                RequiredStringLocal(value, L"record_type");
            record.pageIndex =
                RequiredUInt64Local(value, L"page_index");
            record.logicalOffset =
                RequiredUInt64Local(value, L"logical_offset");
            record.byteLength =
                RequiredUInt64Local(value, L"byte_length");
            record.contentSha256 =
                RequiredStringLocal(value, L"content_sha256");
            record.state =
                RequiredStringLocal(value, L"state");
            record.retentionClass =
                RequiredStringLocal(value, L"retention_class");
            record.committedRootSha256 =
                RequiredStringLocal(
                    value,
                    L"committed_root_sha256");
            record.manifestSha256 =
                RequiredStringLocal(value, L"manifest_sha256");
            record.committed =
                RequiredBoolLocal(value, L"committed");
            record.recordSha256 =
                RequiredStringLocal(value, L"record_sha256");

            if (!IsLowerHex(record.recordSha256, 64) ||
                (!record.previousRecordSha256.empty() &&
                    !IsLowerHex(
                        record.previousRecordSha256,
                        64)) ||
                !record.committed ||
                record.state != L"committed" ||
                (record.retentionClass != L"EPHEMERAL" &&
                    record.retentionClass != L"CACHE" &&
                    record.retentionClass != L"RECOVERY" &&
                    record.retentionClass != L"PINNED") ||
                record.recordSha256 !=
                    HashWide(RecordCanonical(binding, record)))
            {
                throw StoreError(
                    L"JOURNAL_RECORD_HASH_INVALID",
                    "journal record hash or committed state is invalid");
            }
            return record;
        }

        std::vector<uint8_t> PageBytes(
            std::wstring const& namespaceId,
            uint64_t pageIndex,
            uint32_t salt = 0)
        {
            auto seedText =
                namespaceId + L":" +
                std::to_wstring(pageIndex) + L":" +
                std::to_wstring(salt);
            auto seedHash = HashWide(seedText);
            std::vector<uint8_t> bytes(LogicalPageBytes);
            uint32_t state = 0x9e3779b9u;
            for (auto ch : seedHash)
            {
                state = (state * 33u) ^
                    static_cast<uint32_t>(ch);
            }
            for (size_t index = 0; index != bytes.size(); ++index)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                bytes[index] =
                    static_cast<uint8_t>(
                        (state >> ((index & 3u) * 8u)) &
                        0xffu);
            }
            return bytes;
        }

        WorkerPersistentStorageBinding FixtureBinding(
            WorkerPersistentStorageBinding const& binding,
            std::wstring const& fixture)
        {
            auto value = binding;
            value.namespaceId =
                binding.namespaceId + L"-" + fixture;
            if (value.namespaceId.size() > 64)
            {
                throw StoreError(
                    L"NAMESPACE_INVALID",
                    "derived fixture namespace is too long");
            }
            return value;
        }

        uint64_t NextSequence(
            fs::path const& namespaceRoot,
            WorkerPersistentStorageBinding const& binding)
        {
            for (uint64_t sequence = 1;
                 sequence <= binding.maximumJournalRecords;
                 ++sequence)
            {
                if (!fs::exists(
                        JournalRecordPath(
                            namespaceRoot,
                            sequence)))
                {
                    return sequence;
                }
            }
            throw StoreError(
                L"JOURNAL_RECORD_BOUND_EXCEEDED",
                "storage journal record bound was exceeded");
        }

        uint64_t CommittedUniqueBytes(
            fs::path const& namespaceRoot)
        {
            std::map<std::wstring, uint64_t> unique;
            auto pageRoot = namespaceRoot / L"pages";
            if (!fs::exists(pageRoot))
            {
                return 0;
            }
            for (auto const& entry :
                 fs::directory_iterator(pageRoot))
            {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != L".json")
                {
                    continue;
                }
                auto value = ReadJson(entry.path());
                unique[
                    RequiredStringLocal(
                        value,
                        L"content_sha256")] =
                    RequiredUInt64Local(
                        value,
                        L"byte_length");
            }
            uint64_t total = 0;
            for (auto const& item : unique)
            {
                if (item.second >
                    (std::numeric_limits<uint64_t>::max)() -
                        total)
                {
                    throw StoreError(
                        L"QUOTA_ARITHMETIC_OVERFLOW",
                        "storage byte accounting overflowed");
                }
                total += item.second;
            }
            return total;
        }

        std::wstring LastRecordHash(
            fs::path const& namespaceRoot,
            WorkerPersistentStorageBinding const& binding)
        {
            auto next = NextSequence(namespaceRoot, binding);
            if (next == 1)
            {
                return L"";
            }
            return ParseRecord(
                ReadJson(
                    JournalRecordPath(
                        namespaceRoot,
                        next - 1)),
                binding).recordSha256;
        }

        PageCommit CommitPage(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            uint64_t pageIndex,
            std::vector<uint8_t> const& bytes,
            std::wstring const& retentionClass,
            WorkerPersistentAppendCursor* cursor = nullptr)
        {
            if (bytes.empty() ||
                bytes.size() > binding.logicalPageBytes ||
                pageIndex >= binding.maximumPageCount)
            {
                throw StoreError(
                    L"PAGE_BOUNDS_INVALID",
                    "page geometry is invalid");
            }
            auto namespaceRoot =
                NamespaceRoot(workerRoot, binding.namespaceId);
            auto pagePath =
                PageRecordPath(namespaceRoot, pageIndex);
            auto contentSha256 =
                WorkerContentSha256(bytes);
            if (fs::exists(pagePath))
            {
                if (cursor != nullptr)
                {
                    throw StoreError(
                        L"APPEND_CURSOR_STALE",
                        "append cursor points at an existing logical page");
                }
                auto existing = ReadJson(pagePath);
                if (RequiredStringLocal(
                        existing,
                        L"content_sha256") ==
                        contentSha256 &&
                    RequiredUInt64Local(
                        existing,
                        L"byte_length") ==
                        bytes.size())
                {
                    PageCommit replay;
                    replay.contentSha256 = contentSha256;
                    replay.logicalReplay = true;
                    return replay;
                }
                throw StoreError(
                    L"DUPLICATE_LOGICAL_PAGE_IDENTITY",
                    "logical page identity cannot change content");
            }

            auto nextSequence =
                cursor == nullptr
                    ? NextSequence(namespaceRoot, binding)
                    : cursor->nextJournalSequence;
            if (cursor != nullptr &&
                (pageIndex != cursor->nextPageIndex ||
                 nextSequence == 0 ||
                 nextSequence >
                    binding.maximumJournalRecords))
            {
                throw StoreError(
                    L"APPEND_CURSOR_INVALID",
                    "append cursor does not match the admitted page or journal bounds");
            }
            if (nextSequence > 1)
            {
                if (cursor == nullptr)
                {
                    auto lastRecord =
                        ParseRecord(
                            ReadJson(
                                JournalRecordPath(
                                    namespaceRoot,
                                    nextSequence - 1)),
                            binding);
                    if (lastRecord.recordType == L"ROOT_COMMIT")
                    {
                        throw StoreError(
                            L"ROOT_ALREADY_COMMITTED",
                            "a committed root is terminal");
                    }
                }
            }

            auto currentBytes =
                cursor == nullptr
                    ? CommittedUniqueBytes(namespaceRoot)
                    : cursor->committedUniqueBytes;
            auto blobAlreadyExists = fs::exists(
                ArtifactBlobPath(workerRoot, contentSha256));
            auto physicalGrowth =
                blobAlreadyExists ? 0 :
                static_cast<uint64_t>(bytes.size());
            if (physicalGrowth >
                    binding.maximumBytes ||
                currentBytes >
                    binding.maximumBytes -
                        physicalGrowth)
            {
                throw StoreError(
                    L"QUOTA_EXHAUSTED",
                    "page commit exceeds the admitted namespace quota");
            }

            auto stagingToken =
                binding.namespaceId.substr(
                    0,
                    (std::min<size_t>)(
                        binding.namespaceId.size(),
                        40)) +
                L"-p" + std::to_wstring(pageIndex);
            auto pendingPath =
                namespaceRoot /
                L"pending" /
                (std::to_wstring(pageIndex) + L".json");
            JsonObject pending;
            PutString(
                pending,
                L"schema_version",
                L"xcp-content-addressed-pending-page-v1");
            PutNumber(
                pending,
                L"page_index",
                pageIndex);
            PutString(
                pending,
                L"content_sha256",
                contentSha256);
            WriteJson(pendingPath, pending);
            auto blob = WorkerCommitContentAddressedBlob(
                workerRoot,
                stagingToken,
                bytes);

            JsonObject page;
            PutString(page, L"schema_version", PageSchema);
            PutString(page, L"job_id", binding.jobId);
            PutString(
                page,
                L"execution_id",
                binding.executionId);
            PutString(page, L"dataset_id", binding.datasetId);
            PutString(
                page,
                L"execution_plan_sha256",
                binding.executionPlanSha256);
            PutString(
                page,
                L"storage_plan_sha256",
                binding.storagePlanSha256);
            PutString(
                page,
                L"reservation_id",
                binding.reservationId);
            PutNumber(page, L"generation", binding.generation);
            PutNumber(page, L"page_index", pageIndex);
            PutNumber(
                page,
                L"logical_offset",
                pageIndex * binding.logicalPageBytes);
            PutNumber(
                page,
                L"byte_length",
                static_cast<uint64_t>(bytes.size()));
            PutString(
                page,
                L"content_sha256",
                blob.sha256);
            PutString(page, L"state", L"committed");
            PutString(
                page,
                L"retention_class",
                retentionClass);
            WriteJson(pagePath, page);

            JournalRecord record;
            record.sequence = nextSequence;
            record.previousRecordSha256 =
                cursor == nullptr
                    ? LastRecordHash(namespaceRoot, binding)
                    : cursor->previousRecordSha256;
            record.recordType = L"PAGE_COMMIT";
            record.pageIndex = pageIndex;
            record.logicalOffset =
                pageIndex * binding.logicalPageBytes;
            record.byteLength =
                static_cast<uint64_t>(bytes.size());
            record.contentSha256 = blob.sha256;
            record.state = L"committed";
            record.retentionClass = retentionClass;
            auto journal = RecordJson(binding, record);
            WriteJson(
                JournalRecordPath(
                    namespaceRoot,
                    record.sequence),
                journal);
            std::error_code pendingCleanupError;
            fs::remove(
                pendingPath,
                pendingCleanupError);
            fs::remove(
                pendingPath.parent_path(),
                pendingCleanupError);

            PageCommit result;
            result.sequence = record.sequence;
            result.recordSha256 =
                RequiredStringLocal(
                    journal,
                    L"record_sha256");
            result.contentSha256 = blob.sha256;
            result.deduplicated = blob.deduplicated;
            if (cursor != nullptr)
            {
                cursor->nextPageIndex = pageIndex + 1;
                cursor->nextJournalSequence =
                    record.sequence + 1;
                cursor->previousRecordSha256 =
                    result.recordSha256;
                if (!blobAlreadyExists)
                {
                    cursor->committedUniqueBytes +=
                        static_cast<uint64_t>(bytes.size());
                }
            }
            return result;
        }

        std::wstring RootCanonical(
            WorkerPersistentStorageBinding const& binding,
            std::vector<std::wstring> const& pageHashes)
        {
            std::wostringstream text;
            text << RootSchema << L"\n" <<
                binding.jobId << L"\n" <<
                binding.executionId << L"\n" <<
                binding.datasetId << L"\n" <<
                binding.executionPlanSha256 << L"\n" <<
                binding.storagePlanSha256 << L"\n" <<
                binding.generation << L"\n";
            for (auto const& hash : pageHashes)
            {
                text << hash << L"\n";
            }
            return text.str();
        }

        JournalRecord CommitRoot(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            std::vector<std::wstring> const& pageHashes)
        {
            if (pageHashes.empty())
            {
                throw StoreError(
                    L"EMPTY_ROOT_INVALID",
                    "committed page root cannot be empty");
            }
            auto namespaceRoot =
                NamespaceRoot(workerRoot, binding.namespaceId);
            auto nextSequence = NextSequence(namespaceRoot, binding);
            if (nextSequence > 1)
            {
                auto lastRecord =
                    ParseRecord(
                        ReadJson(
                            JournalRecordPath(
                                namespaceRoot,
                                nextSequence - 1)),
                        binding);
                if (lastRecord.recordType == L"ROOT_COMMIT")
                {
                    throw StoreError(
                        L"ROOT_ALREADY_COMMITTED",
                        "a committed root is terminal");
                }
            }
            auto rootSha256 =
                HashWide(RootCanonical(binding, pageHashes));
            JsonObject manifest;
            PutString(
                manifest,
                L"schema_version",
                RootSchema);
            PutString(manifest, L"job_id", binding.jobId);
            PutString(
                manifest,
                L"execution_id",
                binding.executionId);
            PutString(
                manifest,
                L"dataset_id",
                binding.datasetId);
            PutString(
                manifest,
                L"execution_plan_sha256",
                binding.executionPlanSha256);
            PutString(
                manifest,
                L"storage_plan_sha256",
                binding.storagePlanSha256);
            PutNumber(
                manifest,
                L"generation",
                binding.generation);
            JsonArray pages;
            for (auto const& hash : pageHashes)
            {
                pages.Append(
                    JsonValue::CreateStringValue(hash));
            }
            manifest.SetNamedValue(L"page_sha256", pages);
            PutString(
                manifest,
                L"root_sha256",
                rootSha256);
            auto manifestSha256 =
                WorkerContentSha256(
                    to_string(manifest.Stringify()));
            PutString(
                manifest,
                L"manifest_sha256",
                manifestSha256);
            WriteJson(
                namespaceRoot / L"root.json",
                manifest);

            JournalRecord record;
            record.sequence = nextSequence;
            record.previousRecordSha256 =
                LastRecordHash(namespaceRoot, binding);
            record.recordType = L"ROOT_COMMIT";
            record.pageIndex =
                static_cast<uint64_t>(pageHashes.size());
            record.byteLength =
                static_cast<uint64_t>(
                    pageHashes.size()) *
                    binding.logicalPageBytes;
            record.contentSha256 = rootSha256;
            record.state = L"committed";
            record.retentionClass = L"RECOVERY";
            record.committedRootSha256 = rootSha256;
            record.manifestSha256 = manifestSha256;
            auto journal = RecordJson(binding, record);
            WriteJson(
                JournalRecordPath(
                    namespaceRoot,
                    record.sequence),
                journal);
            record.recordSha256 =
                RequiredStringLocal(
                    journal,
                    L"record_sha256");
            return record;
        }

        void ValidatePageRecord(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            JournalRecord const& record)
        {
            auto namespaceRoot =
                NamespaceRoot(workerRoot, binding.namespaceId);
            auto path =
                PageRecordPath(
                    namespaceRoot,
                    record.pageIndex);
            if (!fs::exists(path))
            {
                throw StoreError(
                    L"PAGE_RECORD_MISSING",
                    "committed page record is missing");
            }
            auto page = ReadJson(path);
            if (RequiredStringLocal(
                    page,
                    L"schema_version") != PageSchema ||
                RequiredStringLocal(page, L"job_id") !=
                    binding.jobId ||
                RequiredStringLocal(
                    page,
                    L"execution_id") !=
                    binding.executionId ||
                RequiredStringLocal(page, L"dataset_id") !=
                    binding.datasetId ||
                RequiredStringLocal(
                    page,
                    L"execution_plan_sha256") !=
                    binding.executionPlanSha256 ||
                RequiredStringLocal(
                    page,
                    L"storage_plan_sha256") !=
                    binding.storagePlanSha256 ||
                RequiredStringLocal(
                    page,
                    L"reservation_id") !=
                    binding.reservationId ||
                RequiredUInt64Local(page, L"generation") !=
                    binding.generation ||
                RequiredUInt64Local(page, L"page_index") !=
                    record.pageIndex ||
                RequiredUInt64Local(
                    page,
                    L"logical_offset") !=
                    record.logicalOffset ||
                RequiredUInt64Local(
                    page,
                    L"byte_length") !=
                    record.byteLength ||
                RequiredStringLocal(
                    page,
                    L"content_sha256") !=
                    record.contentSha256 ||
                RequiredStringLocal(page, L"state") !=
                    L"committed" ||
                RequiredStringLocal(
                    page,
                    L"retention_class") !=
                    record.retentionClass)
            {
                throw StoreError(
                    L"PAGE_RECORD_BINDING_INVALID",
                    "page record does not match its journal commit");
            }
            auto blob =
                ArtifactBlobPath(
                    workerRoot,
                    record.contentSha256);
            if (!fs::exists(blob))
            {
                throw StoreError(
                    L"PAGE_CONTENT_MISSING",
                    "committed page content is missing");
            }
            if (static_cast<uint64_t>(
                    fs::file_size(blob)) !=
                    record.byteLength ||
                WorkerContentSha256File(blob) !=
                    record.contentSha256)
            {
                throw StoreError(
                    L"PAGE_CONTENT_HASH_INVALID",
                    "committed page content failed verification");
            }
        }

        void ValidateRoot(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            JournalRecord const& record,
            std::vector<std::wstring> const& pageHashes)
        {
            auto path =
                NamespaceRoot(
                    workerRoot,
                    binding.namespaceId) /
                L"root.json";
            if (!fs::exists(path))
            {
                throw StoreError(
                    L"ROOT_MANIFEST_MISSING",
                    "committed root manifest is missing");
            }
            auto manifest = ReadJson(path);
            if (RequiredStringLocal(
                    manifest,
                    L"schema_version") != RootSchema ||
                RequiredStringLocal(
                    manifest,
                    L"job_id") != binding.jobId ||
                RequiredStringLocal(
                    manifest,
                    L"execution_id") !=
                    binding.executionId ||
                RequiredStringLocal(
                    manifest,
                    L"dataset_id") !=
                    binding.datasetId ||
                RequiredStringLocal(
                    manifest,
                    L"execution_plan_sha256") !=
                    binding.executionPlanSha256 ||
                RequiredStringLocal(
                    manifest,
                    L"storage_plan_sha256") !=
                    binding.storagePlanSha256 ||
                RequiredUInt64Local(
                    manifest,
                    L"generation") !=
                    binding.generation ||
                !manifest.HasKey(L"page_sha256") ||
                manifest.GetNamedValue(
                    L"page_sha256").ValueType() !=
                    JsonValueType::Array)
            {
                throw StoreError(
                    L"ROOT_MANIFEST_INVALID",
                    "committed root manifest binding is invalid");
            }
            auto pages =
                manifest.GetNamedArray(L"page_sha256");
            if (pages.Size() != pageHashes.size())
            {
                throw StoreError(
                    L"ROOT_MANIFEST_INCOMPLETE",
                    "committed root manifest is incomplete");
            }
            for (uint32_t index = 0;
                 index != pages.Size();
                 ++index)
            {
                if (std::wstring(
                        pages.GetStringAt(index).c_str()) !=
                    pageHashes[index])
                {
                    throw StoreError(
                        L"ROOT_MANIFEST_PAGE_MISMATCH",
                        "committed root page order changed");
                }
            }
            auto rootSha256 =
                HashWide(
                    RootCanonical(
                        binding,
                        pageHashes));
            auto manifestSha256 =
                RequiredStringLocal(
                    manifest,
                    L"manifest_sha256");
            auto manifestForHash = JsonObject::Parse(
                manifest.Stringify());
            manifestForHash.Remove(
                L"manifest_sha256");
            auto actualManifestSha256 =
                WorkerContentSha256(
                    to_string(
                        manifestForHash.Stringify()));
            if (RequiredStringLocal(
                    manifest,
                    L"root_sha256") !=
                    rootSha256 ||
                record.contentSha256 != rootSha256 ||
                record.committedRootSha256 != rootSha256 ||
                record.manifestSha256 != manifestSha256 ||
                actualManifestSha256 != manifestSha256)
            {
                throw StoreError(
                    L"ROOT_MANIFEST_HASH_INVALID",
                    "committed root hash or manifest hash is invalid");
            }
        }

        RecoverySnapshot Recover(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding)
        {
            ValidateBinding(binding);
            auto namespaceRoot =
                NamespaceRoot(workerRoot, binding.namespaceId);
            fs::create_directories(namespaceRoot);
            auto lockPath =
                namespaceRoot / L"recovery.lock";
            WriteText(lockPath, "active", true);

            RecoverySnapshot snapshot;
            snapshot.recoveryLockObserved =
                fs::exists(lockPath);
            std::wstring expectedPrevious;
            std::vector<std::wstring> pageHashes;
            std::map<uint64_t, std::wstring> logicalPages;
            for (uint64_t sequence = 1;
                 sequence <= binding.maximumJournalRecords;
                 ++sequence)
            {
                auto path =
                    JournalRecordPath(
                        namespaceRoot,
                        sequence);
                if (!fs::exists(path))
                {
                    break;
                }
                try
                {
                    auto record =
                        ParseRecord(
                            ReadJson(path),
                            binding);
                    if (record.sequence != sequence)
                    {
                        throw StoreError(
                            L"JOURNAL_SEQUENCE_INVALID",
                            "journal sequence is not contiguous");
                    }
                    if (record.previousRecordSha256 !=
                        expectedPrevious)
                    {
                        throw StoreError(
                            L"JOURNAL_CHAIN_INVALID",
                            "journal previous-record hash changed");
                    }
                    if (!snapshot.committedRootSha256.empty())
                    {
                        throw StoreError(
                            L"JOURNAL_RECORD_AFTER_ROOT",
                            "a journal record follows the terminal root");
                    }
                    if (record.recordType ==
                        L"PAGE_COMMIT")
                    {
                        if (logicalPages.find(
                                record.pageIndex) !=
                            logicalPages.end())
                        {
                            throw StoreError(
                                L"DUPLICATE_LOGICAL_PAGE_IDENTITY",
                                "journal repeats a logical page identity");
                        }
                        ValidatePageRecord(
                            workerRoot,
                            binding,
                            record);
                        logicalPages[record.pageIndex] =
                            record.contentSha256;
                        pageHashes.push_back(
                            record.contentSha256);
                        snapshot.orderedPageSha256.push_back(
                            record.contentSha256);
                        snapshot.orderedPageIndices.push_back(
                            record.pageIndex);
                        ++snapshot.committedPageCount;
                        snapshot.committedBytes +=
                            record.byteLength;
                        snapshot.firstUncommittedPageIndex =
                            record.pageIndex + 1;
                    }
                    else if (record.recordType ==
                        L"ROOT_COMMIT")
                    {
                        ValidateRoot(
                            workerRoot,
                            binding,
                            record,
                            pageHashes);
                        snapshot.committedRootSha256 =
                            record.committedRootSha256;
                        snapshot.publicationEligible = true;
                    }
                    else
                    {
                        throw StoreError(
                            L"JOURNAL_RECORD_TYPE_INVALID",
                            "journal record type is invalid");
                    }
                    expectedPrevious =
                        record.recordSha256;
                    snapshot.chainHeadSha256 =
                        record.recordSha256;
                    snapshot.validPrefixLength =
                        sequence;
                }
                catch (StoreError const& error)
                {
                    snapshot.stopCode = error.code;
                    snapshot.invalidTailQuarantined = true;
                    snapshot.publicationEligible =
                        !snapshot.committedRootSha256.empty();
                    break;
                }
            }
            fs::remove(lockPath);
            return snapshot;
        }

        bool TryEvictDuringRecovery(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding)
        {
            auto namespaceRoot =
                NamespaceRoot(workerRoot, binding.namespaceId);
            if (fs::exists(
                    namespaceRoot /
                    L"recovery.lock"))
            {
                return false;
            }
            return true;
        }

        JsonObject SnapshotJson(
            RecoverySnapshot const& snapshot)
        {
            JsonObject value;
            PutNumber(
                value,
                L"valid_prefix_length",
                snapshot.validPrefixLength);
            PutNumber(
                value,
                L"committed_page_count",
                snapshot.committedPageCount);
            PutNumber(
                value,
                L"committed_bytes",
                snapshot.committedBytes);
            PutNumber(
                value,
                L"first_uncommitted_page_index",
                snapshot.firstUncommittedPageIndex);
            PutString(
                value,
                L"chain_head_sha256",
                snapshot.chainHeadSha256);
            PutString(
                value,
                L"committed_root_sha256",
                snapshot.committedRootSha256);
            PutString(
                value,
                L"stop_code",
                snapshot.stopCode);
            PutBool(
                value,
                L"invalid_tail_quarantined",
                snapshot.invalidTailQuarantined);
            PutBool(
                value,
                L"publication_eligible",
                snapshot.publicationEligible);
            PutBool(
                value,
                L"recovery_lock_observed",
                snapshot.recoveryLockObserved);
            return value;
        }

        JsonObject Cleanup(
            fs::path const& workerRoot,
            std::wstring const& namespaceId)
        {
            auto namespaceRoot =
                NamespaceRoot(workerRoot, namespaceId);
            std::vector<std::wstring> hashes;
            if (fs::exists(namespaceRoot))
            {
                for (auto const& entry :
                     fs::recursive_directory_iterator(
                         namespaceRoot))
                {
                    if (!entry.is_regular_file() ||
                        entry.path().extension() != L".json")
                    {
                        continue;
                    }
                    try
                    {
                        auto value = ReadJson(entry.path());
                        if (value.HasKey(L"content_sha256") &&
                            value.GetNamedValue(
                                L"content_sha256").ValueType() ==
                                JsonValueType::String)
                        {
                            auto hash = std::wstring(
                                value.GetNamedString(
                                    L"content_sha256").c_str());
                            if (IsLowerHex(hash, 64))
                            {
                                hashes.push_back(hash);
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(
                std::unique(
                    hashes.begin(),
                    hashes.end()),
                hashes.end());
            uint64_t removedEntries = 0;
            if (fs::exists(namespaceRoot))
            {
                removedEntries =
                    static_cast<uint64_t>(
                        fs::remove_all(namespaceRoot));
            }
            auto blobCleanup =
                WorkerRemoveUnreferencedContentAddressedBlobs(
                    workerRoot,
                    hashes);
            JsonObject value;
            PutBool(value, L"namespace_existed", removedEntries != 0);
            PutNumber(
                value,
                L"removed_entries",
                removedEntries);
            PutNumber(
                value,
                L"removed_unreferenced_blobs",
                blobCleanup.removedBlobCount);
            PutNumber(
                value,
                L"removed_unreferenced_blob_bytes",
                blobCleanup.removedBytes);
            PutBool(
                value,
                L"blob_cleanup_protected_by_unreadable_metadata",
                blobCleanup.protectedByUnreadableMetadata);
            PutBool(
                value,
                L"namespace_absent",
                !fs::exists(namespaceRoot));
            return value;
        }

        void ResetFixture(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding)
        {
            Cleanup(workerRoot, binding.namespaceId);
        }

        std::vector<std::wstring> BuildOnePageRoot(
            fs::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            uint32_t salt = 0)
        {
            auto page =
                CommitPage(
                    workerRoot,
                    binding,
                    0,
                    PageBytes(
                        binding.namespaceId,
                        0,
                        salt),
                    L"RECOVERY");
            std::vector<std::wstring> hashes{
                page.contentSha256
            };
            CommitRoot(workerRoot, binding, hashes);
            return hashes;
        }

        JsonObject FixtureResult(
            std::wstring const& name,
            bool passed,
            std::wstring const& expected,
            std::wstring const& observed,
            RecoverySnapshot const* recovery = nullptr)
        {
            JsonObject value;
            PutString(value, L"name", name);
            PutBool(value, L"passed", passed);
            PutString(value, L"expected", expected);
            PutString(value, L"observed", observed);
            if (recovery)
            {
                value.SetNamedValue(
                    L"recovery",
                    SnapshotJson(*recovery));
            }
            return value;
        }

        void RequireFixture(
            bool condition,
            std::wstring const& name)
        {
            if (!condition)
            {
                throw StoreError(
                    L"MATRIX_FIXTURE_FAILED",
                    "storage recovery matrix fixture failed: " +
                        to_string(hstring(name)));
            }
        }
    }

    JsonObject WorkerContentAddressedStoreRecovery::RunCoreMatrix(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding) const
    {
        ValidateBinding(binding);
        JsonArray positiveChecks;
        JsonArray faultChecks;
        std::vector<std::wstring> fixtureNamespaces;

        auto positive =
            FixtureBinding(binding, L"positive");
        fixtureNamespaces.push_back(positive.namespaceId);
        ResetFixture(workerRoot, positive);
        auto page0Bytes =
            PageBytes(positive.namespaceId, 0);
        auto page0 =
            CommitPage(
                workerRoot,
                positive,
                0,
                page0Bytes,
                L"RECOVERY");
        auto page1 =
            CommitPage(
                workerRoot,
                positive,
                1,
                PageBytes(positive.namespaceId, 1),
                L"CACHE");
        auto page2 =
            CommitPage(
                workerRoot,
                positive,
                2,
                page0Bytes,
                L"PINNED");
        auto page0Replay =
            CommitPage(
                workerRoot,
                positive,
                0,
                page0Bytes,
                L"RECOVERY");
        std::vector<std::wstring> positiveHashes{
            page0.contentSha256,
            page1.contentSha256,
            page2.contentSha256
        };
        auto root =
            CommitRoot(
                workerRoot,
                positive,
                positiveHashes);
        auto positiveRecovery =
            Recover(workerRoot, positive);
        RequireFixture(
            page2.deduplicated &&
            page0Replay.logicalReplay &&
            positiveRecovery.validPrefixLength == 4 &&
            positiveRecovery.committedPageCount == 3 &&
            positiveRecovery.publicationEligible &&
            !positiveRecovery.committedRootSha256.empty() &&
            positiveRecovery.committedRootSha256 ==
                root.committedRootSha256,
            L"positive_page_store_journal");
        positiveChecks.Append(
            FixtureResult(
                L"content_addressed_deduplication",
                page2.deduplicated,
                L"true",
                page2.deduplicated ? L"true" : L"false"));
        positiveChecks.Append(
            FixtureResult(
                L"duplicate_logical_replay_idempotent",
                page0Replay.logicalReplay,
                L"true",
                page0Replay.logicalReplay ?
                    L"true" : L"false"));
        positiveChecks.Append(
            FixtureResult(
                L"committed_root_publication_gate",
                positiveRecovery.publicationEligible,
                L"committed_root_required",
                positiveRecovery.publicationEligible ?
                    L"eligible" : L"blocked",
                &positiveRecovery));

        auto truncated =
            FixtureBinding(binding, L"truncated");
        fixtureNamespaces.push_back(truncated.namespaceId);
        ResetFixture(workerRoot, truncated);
        BuildOnePageRoot(workerRoot, truncated);
        WriteText(
            JournalRecordPath(
                NamespaceRoot(
                    workerRoot,
                    truncated.namespaceId),
                3),
            "{\"schema_version\":",
            false);
        auto truncatedRecovery =
            Recover(workerRoot, truncated);
        RequireFixture(
            truncatedRecovery.validPrefixLength == 2 &&
            truncatedRecovery.stopCode ==
                L"JOURNAL_PARSE_INVALID" &&
            truncatedRecovery.publicationEligible,
            L"truncated_record");
        faultChecks.Append(
            FixtureResult(
                L"truncated_record",
                true,
                L"longest_valid_prefix_2",
                truncatedRecovery.stopCode,
                &truncatedRecovery));

        auto wrongHash =
            FixtureBinding(binding, L"wrong-hash");
        fixtureNamespaces.push_back(wrongHash.namespaceId);
        ResetFixture(workerRoot, wrongHash);
        BuildOnePageRoot(workerRoot, wrongHash);
        auto wrongHashPath =
            JournalRecordPath(
                NamespaceRoot(
                    workerRoot,
                    wrongHash.namespaceId),
                2);
        auto wrongHashJson = ReadJson(wrongHashPath);
        PutString(
            wrongHashJson,
            L"record_sha256",
            std::wstring(64, L'0'));
        WriteJson(wrongHashPath, wrongHashJson, true);
        auto wrongHashRecovery =
            Recover(workerRoot, wrongHash);
        RequireFixture(
            wrongHashRecovery.validPrefixLength == 1 &&
            wrongHashRecovery.stopCode ==
                L"JOURNAL_RECORD_HASH_INVALID" &&
            !wrongHashRecovery.publicationEligible,
            L"wrong_record_hash");
        faultChecks.Append(
            FixtureResult(
                L"wrong_record_hash",
                true,
                L"longest_valid_prefix_1",
                wrongHashRecovery.stopCode,
                &wrongHashRecovery));

        auto missingCommit =
            FixtureBinding(binding, L"missing-commit");
        fixtureNamespaces.push_back(missingCommit.namespaceId);
        ResetFixture(workerRoot, missingCommit);
        CommitPage(
                workerRoot,
                missingCommit,
                0,
                PageBytes(
                    missingCommit.namespaceId,
                    0),
                L"RECOVERY");
        auto missingCommitRecovery =
            Recover(workerRoot, missingCommit);
        RequireFixture(
            missingCommitRecovery.validPrefixLength == 1 &&
            !missingCommitRecovery.publicationEligible &&
            missingCommitRecovery.committedPageCount == 1,
            L"missing_commit");
        faultChecks.Append(
            FixtureResult(
                L"missing_commit",
                true,
                L"publication_blocked",
                missingCommitRecovery.stopCode,
                &missingCommitRecovery));

        auto corruptPage =
            FixtureBinding(binding, L"corrupt-page");
        fixtureNamespaces.push_back(corruptPage.namespaceId);
        ResetFixture(workerRoot, corruptPage);
        auto corruptHashes =
            BuildOnePageRoot(workerRoot, corruptPage);
        auto corruptBlob =
            ArtifactBlobPath(
                workerRoot,
                corruptHashes.front());
        {
            std::fstream output(
                corruptBlob,
                std::ios::binary |
                std::ios::in |
                std::ios::out);
            char changed = 0x5a;
            output.write(&changed, 1);
            output.flush();
        }
        auto corruptPageRecovery =
            Recover(workerRoot, corruptPage);
        RequireFixture(
            corruptPageRecovery.validPrefixLength == 0 &&
            corruptPageRecovery.stopCode ==
                L"PAGE_CONTENT_HASH_INVALID" &&
            !corruptPageRecovery.publicationEligible,
            L"corrupted_page");
        faultChecks.Append(
            FixtureResult(
                L"corrupted_page",
                true,
                L"prefix_0_publication_blocked",
                corruptPageRecovery.stopCode,
                &corruptPageRecovery));

        auto missingPageContent =
            FixtureBinding(binding, L"missing-page");
        fixtureNamespaces.push_back(
            missingPageContent.namespaceId);
        ResetFixture(workerRoot, missingPageContent);
        auto missingHashes =
            BuildOnePageRoot(
                workerRoot,
                missingPageContent);
        fs::remove(
            ArtifactBlobPath(
                workerRoot,
                missingHashes.front()));
        auto missingPageRecovery =
            Recover(
                workerRoot,
                missingPageContent);
        RequireFixture(
            missingPageRecovery.validPrefixLength == 0 &&
            missingPageRecovery.stopCode ==
                L"PAGE_CONTENT_MISSING" &&
            !missingPageRecovery.publicationEligible,
            L"missing_page");
        faultChecks.Append(
            FixtureResult(
                L"missing_page",
                true,
                L"prefix_0_publication_blocked",
                missingPageRecovery.stopCode,
                &missingPageRecovery));

        auto wrongPage =
            FixtureBinding(binding, L"wrong-page");
        fixtureNamespaces.push_back(wrongPage.namespaceId);
        ResetFixture(workerRoot, wrongPage);
        BuildOnePageRoot(workerRoot, wrongPage);
        auto wrongPagePath =
            PageRecordPath(
                NamespaceRoot(
                    workerRoot,
                    wrongPage.namespaceId),
                0);
        auto wrongPageJson =
            ReadJson(wrongPagePath);
        PutString(
            wrongPageJson,
            L"content_sha256",
            std::wstring(64, L'1'));
        WriteJson(
            wrongPagePath,
            wrongPageJson,
            true);
        auto wrongPageRecovery =
            Recover(workerRoot, wrongPage);
        RequireFixture(
            wrongPageRecovery.validPrefixLength == 0 &&
            wrongPageRecovery.stopCode ==
                L"PAGE_RECORD_BINDING_INVALID",
            L"wrong_page_hash");
        faultChecks.Append(
            FixtureResult(
                L"wrong_page_hash",
                true,
                L"prefix_0",
                wrongPageRecovery.stopCode,
                &wrongPageRecovery));

        auto crashCommit =
            FixtureBinding(binding, L"crash-commit");
        fixtureNamespaces.push_back(crashCommit.namespaceId);
        ResetFixture(workerRoot, crashCommit);
        CommitPage(
            workerRoot,
            crashCommit,
            0,
            PageBytes(crashCommit.namespaceId, 0),
            L"RECOVERY");
        auto crashRoot =
            NamespaceRoot(
                workerRoot,
                crashCommit.namespaceId);
        WriteText(
            crashRoot / L"root.json.pending",
            "{\"incomplete\":true}",
            true);
        WriteText(
            crashRoot / L"temp" / L"flush.tmp",
            "orphan",
            true);
        auto crashRecovery =
            Recover(workerRoot, crashCommit);
        RequireFixture(
            crashRecovery.validPrefixLength == 1 &&
            !crashRecovery.publicationEligible,
            L"crash_during_commit");
        faultChecks.Append(
            FixtureResult(
                L"crash_during_commit",
                true,
                L"prefix_1_publication_blocked",
                crashRecovery.stopCode,
                &crashRecovery));

        auto restart =
            FixtureBinding(binding, L"restart");
        fixtureNamespaces.push_back(restart.namespaceId);
        ResetFixture(workerRoot, restart);
        BuildOnePageRoot(workerRoot, restart);
        auto restartRecovery =
            Recover(workerRoot, restart);
        RequireFixture(
            restartRecovery.validPrefixLength == 2 &&
            restartRecovery.publicationEligible,
            L"logical_restart");
        faultChecks.Append(
            FixtureResult(
                L"logical_restart",
                true,
                L"fresh_runtime_recovers_root",
                restartRecovery.stopCode,
                &restartRecovery));

        auto eviction =
            FixtureBinding(binding, L"eviction");
        fixtureNamespaces.push_back(eviction.namespaceId);
        ResetFixture(workerRoot, eviction);
        BuildOnePageRoot(workerRoot, eviction);
        auto evictionRoot =
            NamespaceRoot(
                workerRoot,
                eviction.namespaceId);
        WriteText(
            evictionRoot / L"recovery.lock",
            "active",
            true);
        auto evictionBlocked =
            !TryEvictDuringRecovery(
                workerRoot,
                eviction);
        fs::remove(
            evictionRoot / L"recovery.lock");
        auto evictionRecovery =
            Recover(workerRoot, eviction);
        RequireFixture(
            evictionBlocked &&
            evictionRecovery.publicationEligible,
            L"eviction_during_recovery");
        faultChecks.Append(
            FixtureResult(
                L"eviction_during_recovery",
                true,
                L"blocked",
                evictionBlocked ? L"blocked" : L"allowed",
                &evictionRecovery));

        auto incomplete =
            FixtureBinding(binding, L"incomplete-root");
        fixtureNamespaces.push_back(incomplete.namespaceId);
        ResetFixture(workerRoot, incomplete);
        BuildOnePageRoot(workerRoot, incomplete);
        auto incompleteRootPath =
            NamespaceRoot(
                workerRoot,
                incomplete.namespaceId) /
            L"root.json";
        auto incompleteJson =
            ReadJson(incompleteRootPath);
        incompleteJson.SetNamedValue(
            L"page_sha256",
            JsonArray());
        WriteJson(
            incompleteRootPath,
            incompleteJson,
            true);
        auto incompleteRecovery =
            Recover(workerRoot, incomplete);
        RequireFixture(
            incompleteRecovery.validPrefixLength == 1 &&
            incompleteRecovery.stopCode ==
                L"ROOT_MANIFEST_INCOMPLETE" &&
            !incompleteRecovery.publicationEligible,
            L"incomplete_manifest");
        faultChecks.Append(
            FixtureResult(
                L"incomplete_manifest",
                true,
                L"prefix_1_publication_blocked",
                incompleteRecovery.stopCode,
                &incompleteRecovery));

        auto quota =
            FixtureBinding(binding, L"quota");
        fixtureNamespaces.push_back(quota.namespaceId);
        ResetFixture(workerRoot, quota);
        quota.maximumBytes =
            LogicalPageBytes;
        CommitPage(
            workerRoot,
            quota,
            0,
            PageBytes(quota.namespaceId, 0),
            L"EPHEMERAL");
        std::wstring quotaCode;
        try
        {
            CommitPage(
                workerRoot,
                quota,
                1,
                PageBytes(quota.namespaceId, 1),
                L"EPHEMERAL");
        }
        catch (StoreError const& error)
        {
            quotaCode = error.code;
        }
        RequireFixture(
            quotaCode == L"QUOTA_EXHAUSTED",
            L"quota_exhausted");
        faultChecks.Append(
            FixtureResult(
                L"quota_exhausted",
                true,
                L"QUOTA_EXHAUSTED",
                quotaCode));

        auto conflict =
            FixtureBinding(binding, L"duplicate-page");
        fixtureNamespaces.push_back(conflict.namespaceId);
        ResetFixture(workerRoot, conflict);
        CommitPage(
            workerRoot,
            conflict,
            0,
            PageBytes(conflict.namespaceId, 0),
            L"RECOVERY");
        std::wstring conflictCode;
        try
        {
            CommitPage(
                workerRoot,
                conflict,
                0,
                PageBytes(conflict.namespaceId, 0, 9),
                L"RECOVERY");
        }
        catch (StoreError const& error)
        {
            conflictCode = error.code;
        }
        RequireFixture(
            conflictCode ==
                L"DUPLICATE_LOGICAL_PAGE_IDENTITY",
            L"duplicate_logical_page_identity");
        faultChecks.Append(
            FixtureResult(
                L"duplicate_logical_page_identity",
                true,
                L"DUPLICATE_LOGICAL_PAGE_IDENTITY",
                conflictCode));

        auto staleReservation =
            FixtureBinding(binding, L"stale-reservation");
        fixtureNamespaces.push_back(
            staleReservation.namespaceId);
        ResetFixture(workerRoot, staleReservation);
        BuildOnePageRoot(
            workerRoot,
            staleReservation);
        auto rebound = staleReservation;
        rebound.reservationId =
            L"stale-reservation-must-not-reactivate";
        auto staleRecovery =
            Recover(workerRoot, rebound);
        RequireFixture(
            staleRecovery.validPrefixLength == 0 &&
            staleRecovery.stopCode ==
                L"RESERVATION_BINDING_MISMATCH",
            L"stale_reservation");
        faultChecks.Append(
            FixtureResult(
                L"stale_reservation",
                true,
                L"RESERVATION_BINDING_MISMATCH",
                staleRecovery.stopCode,
                &staleRecovery));

        auto orphan =
            FixtureBinding(binding, L"orphan-temp");
        fixtureNamespaces.push_back(orphan.namespaceId);
        ResetFixture(workerRoot, orphan);
        BuildOnePageRoot(workerRoot, orphan);
        auto orphanPath =
            NamespaceRoot(
                workerRoot,
                orphan.namespaceId) /
            L"temp" / L"orphan.tmp";
        WriteText(orphanPath, "orphan", true);
        auto orphanRecovery =
            Recover(workerRoot, orphan);
        auto orphanCleanup =
            Cleanup(workerRoot, orphan.namespaceId);
        auto orphanRemoved =
            !fs::exists(orphanPath) &&
            orphanCleanup.GetNamedBoolean(
                L"namespace_absent");
        RequireFixture(
            orphanRecovery.publicationEligible &&
            orphanRemoved,
            L"orphan_temp");
        faultChecks.Append(
            FixtureResult(
                L"orphan_temp",
                true,
                L"removed_after_recovery",
                orphanRemoved ? L"removed" : L"present",
                &orphanRecovery));

        auto doubleCleanup =
            FixtureBinding(binding, L"double-cleanup");
        fixtureNamespaces.push_back(
            doubleCleanup.namespaceId);
        ResetFixture(workerRoot, doubleCleanup);
        BuildOnePageRoot(workerRoot, doubleCleanup);
        auto firstCleanup =
            Cleanup(
                workerRoot,
                doubleCleanup.namespaceId);
        auto secondCleanup =
            Cleanup(
                workerRoot,
                doubleCleanup.namespaceId);
        auto cleanupIdempotent =
            firstCleanup.GetNamedBoolean(
                L"namespace_absent") &&
            secondCleanup.GetNamedBoolean(
                L"namespace_absent") &&
            RequiredUInt64Local(
                secondCleanup,
                L"removed_entries") == 0;
        RequireFixture(
            cleanupIdempotent,
            L"double_cleanup");
        faultChecks.Append(
            FixtureResult(
                L"double_cleanup",
                true,
                L"idempotent",
                cleanupIdempotent ?
                    L"idempotent" : L"changed"));

        for (auto const& namespaceId :
             fixtureNamespaces)
        {
            Cleanup(workerRoot, namespaceId);
        }

        JsonObject result;
        PutString(
            result,
            L"schema_version",
            L"xcp-content-addressed-store-recovery-v1-matrix-result");
        PutString(result, L"delivery_train", TrainId);
        PutBool(result, L"ok", true);
        PutString(
            result,
            L"shared_cas_owner",
            L"WorkerArtifactStore");
        PutBool(
            result,
            L"second_artifact_system",
            false);
        PutBool(
            result,
            L"graph_checkpoint_semantics_changed",
            false);
        PutBool(
            result,
            L"worker_final_publication_veto",
            true);
        result.SetNamedValue(
            L"positive_page_store_journal_matrix",
            positiveChecks);
        result.SetNamedValue(
            L"negative_recovery_fault_matrix",
            faultChecks);
        JsonObject subgates;
        PutString(
            subgates,
            L"t6_content_addressed_page_store_and_journal",
            L"PASS");
        PutString(
            subgates,
            L"t7_recovery_and_negative_matrix",
            L"PASS_CORE_MATRIX");
        result.SetNamedValue(L"subgates", subgates);
        return result;
    }

    JsonObject WorkerContentAddressedStoreRecovery::SeedRestartFixture(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding) const
    {
        ValidateBinding(binding);
        ResetFixture(workerRoot, binding);
        BuildOnePageRoot(workerRoot, binding);
        auto recovery = Recover(workerRoot, binding);
        JsonObject result;
        PutString(
            result,
            L"schema_version",
            L"xcp-content-addressed-store-recovery-v1-restart-result");
        PutString(result, L"operation", L"seed_restart");
        PutBool(result, L"ok", recovery.publicationEligible);
        result.SetNamedValue(
            L"recovery",
            SnapshotJson(recovery));
        return result;
    }

    JsonObject WorkerContentAddressedStoreRecovery::RecoverRestartFixture(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding) const
    {
        ValidateBinding(binding);
        auto recovery = Recover(workerRoot, binding);
        JsonObject result;
        PutString(
            result,
            L"schema_version",
            L"xcp-content-addressed-store-recovery-v1-restart-result");
        PutString(result, L"operation", L"recover_restart");
        PutBool(result, L"ok", recovery.publicationEligible);
        result.SetNamedValue(
            L"recovery",
            SnapshotJson(recovery));
        return result;
    }

    JsonObject WorkerContentAddressedStoreRecovery::CleanupNamespace(
        fs::path const& workerRoot,
        std::wstring const& namespaceId) const
    {
        return Cleanup(workerRoot, namespaceId);
    }

    WorkerPersistentRecoverySnapshot
    WorkerContentAddressedStoreRecovery::RecoverCommittedPrefix(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding) const
    {
        auto recovered = Recover(workerRoot, binding);
        WorkerPersistentRecoverySnapshot result;
        result.validPrefixLength = recovered.validPrefixLength;
        result.committedPageCount = recovered.committedPageCount;
        result.committedBytes = recovered.committedBytes;
        result.firstUncommittedPageIndex =
            recovered.firstUncommittedPageIndex;
        result.chainHeadSha256 = recovered.chainHeadSha256;
        result.committedRootSha256 =
            recovered.committedRootSha256;
        result.stopCode = recovered.stopCode;
        result.orderedPageSha256 =
            recovered.orderedPageSha256;
        result.orderedPageIndices =
            recovered.orderedPageIndices;
        result.invalidTailQuarantined =
            recovered.invalidTailQuarantined;
        result.publicationEligible =
            recovered.publicationEligible;
        result.recoveryLockObserved =
            recovered.recoveryLockObserved;
        return result;
    }

    WorkerPersistentPageCommit
    WorkerContentAddressedStoreRecovery::CommitVerifiedPage(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        uint64_t pageIndex,
        std::vector<uint8_t> const& bytes,
        std::wstring const& retentionClass) const
    {
        ValidateBinding(binding);
        auto committed = CommitPage(
            workerRoot,
            binding,
            pageIndex,
            bytes,
            retentionClass);
        WorkerPersistentPageCommit result;
        result.sequence = committed.sequence;
        result.recordSha256 = committed.recordSha256;
        result.contentSha256 = committed.contentSha256;
        result.deduplicated = committed.deduplicated;
        result.logicalReplay = committed.logicalReplay;
        return result;
    }

    WorkerPersistentAppendCursor
    WorkerContentAddressedStoreRecovery::OpenVerifiedAppendCursor(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding) const
    {
        ValidateBinding(binding);
        auto recovered = Recover(workerRoot, binding);
        if (recovered.invalidTailQuarantined ||
            !recovered.committedRootSha256.empty() ||
            recovered.validPrefixLength !=
                recovered.committedPageCount ||
            recovered.firstUncommittedPageIndex !=
                recovered.committedPageCount)
        {
            throw StoreError(
                L"APPEND_PREFIX_INVALID",
                "verified append requires a contiguous rootless journal prefix");
        }
        for (uint64_t index = 0;
             index < recovered.orderedPageIndices.size();
             ++index)
        {
            if (recovered.orderedPageIndices[index] != index)
            {
                throw StoreError(
                    L"APPEND_PREFIX_INVALID",
                    "verified append requires ordered contiguous page identities");
            }
        }

        WorkerPersistentAppendCursor cursor;
        cursor.nextPageIndex =
            recovered.firstUncommittedPageIndex;
        cursor.nextJournalSequence =
            recovered.validPrefixLength + 1;
        cursor.committedUniqueBytes =
            CommittedUniqueBytes(
                NamespaceRoot(
                    workerRoot,
                    binding.namespaceId));
        cursor.previousRecordSha256 =
            recovered.chainHeadSha256;
        return cursor;
    }

    WorkerPersistentPageCommit
    WorkerContentAddressedStoreRecovery::CommitVerifiedPage(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        uint64_t pageIndex,
        std::vector<uint8_t> const& bytes,
        std::wstring const& retentionClass,
        WorkerPersistentAppendCursor& cursor) const
    {
        ValidateBinding(binding);
        auto committed = CommitPage(
            workerRoot,
            binding,
            pageIndex,
            bytes,
            retentionClass,
            &cursor);
        WorkerPersistentPageCommit result;
        result.sequence = committed.sequence;
        result.recordSha256 = committed.recordSha256;
        result.contentSha256 = committed.contentSha256;
        result.deduplicated = committed.deduplicated;
        result.logicalReplay = committed.logicalReplay;
        return result;
    }

    WorkerPersistentRootCommit
    WorkerContentAddressedStoreRecovery::CommitVerifiedRoot(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        std::vector<std::wstring> const& orderedPageSha256) const
    {
        ValidateBinding(binding);
        if (orderedPageSha256.size() !=
            binding.maximumPageCount)
        {
            throw StoreError(
                L"ROOT_PAGE_COUNT_INVALID",
                "the ordered root does not contain the admitted page count");
        }
        auto committed = CommitRoot(
            workerRoot,
            binding,
            orderedPageSha256);
        WorkerPersistentRootCommit result;
        result.sequence = committed.sequence;
        result.recordSha256 = committed.recordSha256;
        result.rootSha256 = committed.committedRootSha256;
        result.manifestSha256 = committed.manifestSha256;
        return result;
    }

    std::vector<uint8_t>
    WorkerContentAddressedStoreRecovery::ReadVerifiedCommittedPage(
        fs::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        uint64_t pageIndex,
        std::wstring const& expectedSha256) const
    {
        ValidateBinding(binding);
        if (pageIndex >= binding.maximumPageCount ||
            !IsLowerHex(expectedSha256, 64))
        {
            throw StoreError(
                L"PAGE_READ_IDENTITY_INVALID",
                "the requested page identity is invalid");
        }
        auto namespaceRoot =
            NamespaceRoot(workerRoot, binding.namespaceId);
        auto pagePath =
            PageRecordPath(namespaceRoot, pageIndex);
        if (!fs::exists(pagePath))
        {
            throw StoreError(
                L"PAGE_RECORD_MISSING",
                "the requested committed page record is absent");
        }
        auto page = ReadJson(pagePath);
        JournalRecord record;
        record.pageIndex = pageIndex;
        record.logicalOffset =
            RequiredUInt64Local(page, L"logical_offset");
        record.byteLength =
            RequiredUInt64Local(page, L"byte_length");
        record.contentSha256 =
            RequiredStringLocal(page, L"content_sha256");
        record.state =
            RequiredStringLocal(page, L"state");
        record.retentionClass =
            RequiredStringLocal(page, L"retention_class");
        if (record.contentSha256 != expectedSha256 ||
            record.byteLength != binding.logicalPageBytes ||
            record.logicalOffset !=
                pageIndex * binding.logicalPageBytes)
        {
            throw StoreError(
                L"PAGE_READ_IDENTITY_MISMATCH",
                "the requested page differs from the committed identity");
        }
        ValidatePageRecord(workerRoot, binding, record);
        auto blobPath =
            ArtifactBlobPath(workerRoot, expectedSha256);
        std::ifstream input(blobPath, std::ios::binary);
        if (!input)
        {
            throw StoreError(
                L"PAGE_CONTENT_MISSING",
                "the verified committed page content is absent");
        }
        std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (bytes.size() != binding.logicalPageBytes ||
            WorkerContentSha256(bytes) != expectedSha256)
        {
            throw StoreError(
                L"PAGE_CONTENT_HASH_INVALID",
                "the committed page changed while it was read");
        }
        return bytes;
    }

    std::wstring WorkerContentAddressedStoreRecoveryCapabilityJson()
    {
        JsonObject value;
        PutString(
            value,
            L"schema_version",
            L"xcp-content-addressed-store-recovery-v1-capability");
        PutString(value, L"delivery_train", TrainId);
        PutString(
            value,
            L"shared_cas_owner",
            L"WorkerArtifactStore");
        PutBool(value, L"page_store_materialized", true);
        PutBool(
            value,
            L"persistent_journal_materialized",
            true);
        PutBool(
            value,
            L"longest_valid_prefix_recovery",
            true);
        PutBool(
            value,
            L"idempotent_cleanup",
            true);
        PutBool(
            value,
            L"second_artifact_system",
            false);
        PutBool(
            value,
            L"graph_checkpoint_semantics_changed",
            false);
        PutBool(
            value,
            L"worker_final_publication_veto",
            true);
        PutString(
            value,
            L"lifecycle_measurement_status",
            L"CORE_MATRIX_LOCAL_LIVE_PENDING");
        return std::wstring(value.Stringify().c_str());
    }
}
