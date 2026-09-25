#include "pch.h"
#include "WorkerProvableWorldsStreamingRuntime.h"

#include "WorkerCpuGpuConvergenceBackend.h"
#include "WorkerProcessTopologyMemoryProbe.h"
#include "WorkerProcessTopologyProtocol.h"

#include <array>

#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe
{
    namespace
    {
        using namespace ProcessTopology;

        constexpr wchar_t const* ContractId =
            L"PROVABLE_WORLDS_STREAMING_PRODUCT_ADMISSION_V1";
        constexpr wchar_t const* ProgramId =
            L"provable_worlds_field_v1";
        constexpr wchar_t const* ProgramSha256 =
            L"5c8c06aa8700f0dcec90263c5a4243cd4ecfebbaaec25e870074e1906e8fc836";
        constexpr wchar_t const* ProfileId =
            L"provable_worlds_cpu_capsule_v1";
        constexpr wchar_t const* ProfileContractSha256 =
            L"e0b6a59c07b7225135efb5fce4b0f72f18ddb903325fdbdf35b038415f366486";
        constexpr wchar_t const* BoundInputSha256 =
            L"3eecbcff36b8542da2fd554693ed3dc349c9e0a381adb42c70541a9a816b81d1";
        constexpr wchar_t const* BoundInputMix64 =
            L"60b1b806ed391db2";
        constexpr wchar_t const* ExpectedFieldSha256 =
            L"24cd52a3e9b9f2014ba020a9fdbaecce7a4604a6ee47a336739484d168458bef";
        constexpr uint32_t InputSeed = 0x60b1b806u;
        constexpr uint32_t GridWidth = 1024;
        constexpr uint32_t GridHeight = 1024;
        constexpr uint32_t TileHeight = 4;
        constexpr uint32_t TileLanes = GridWidth * TileHeight;
        constexpr uint32_t TileBytes = TileLanes * sizeof(uint32_t);
        constexpr uint32_t TileCount = GridHeight / TileHeight;
        constexpr uint32_t StopBeforeCommitTileIndex = 128;
        constexpr uint32_t MaximumSimultaneousBuffers = 4;
        constexpr uint64_t FieldBytes =
            static_cast<uint64_t>(TileBytes) * TileCount;

        double ElapsedMilliseconds(
            std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }

        uint64_t DirectoryBytes(
            std::filesystem::path const& root)
        {
            uint64_t total = 0;
            if (!std::filesystem::exists(root))
            {
                return total;
            }
            for (auto const& entry :
                 std::filesystem::recursive_directory_iterator(root))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                auto bytes =
                    static_cast<uint64_t>(
                        entry.file_size());
                if (bytes >
                    (std::numeric_limits<uint64_t>::max)() -
                        total)
                {
                    throw WorkerProvableWorldsStreamingError(
                        L"T8_METRIC_OVERFLOW",
                        "the bounded storage metric overflowed");
                }
                total += bytes;
            }
            return total;
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

        uint32_t RotateLeft13(uint32_t value)
        {
            return
                static_cast<uint32_t>(value << 13) |
                static_cast<uint32_t>(value >> 19);
        }

        void BuildCanonicalTile(
            uint32_t tileIndex,
            std::vector<uint8_t>& bytes)
        {
            if (tileIndex >= TileCount)
            {
                throw WorkerProvableWorldsStreamingError(
                    L"T8_TILE_INDEX_INVALID",
                    "the requested Provable Worlds tile is outside the admitted field");
            }
            bytes.resize(TileBytes);
            auto startLane = tileIndex * TileLanes;
            for (uint32_t localLane = 0;
                 localLane != TileLanes;
                 ++localLane)
            {
                auto globalLane = startLane + localLane;
                auto x = globalLane % GridWidth;
                auto y = globalLane / GridWidth;
                auto xMix =
                    static_cast<uint32_t>(x * 374761393u);
                auto yMix =
                    static_cast<uint32_t>(y * 668265263u);
                auto combined =
                    static_cast<uint32_t>(
                        xMix ^ yMix ^ InputSeed);
                auto result =
                    static_cast<uint32_t>(
                        RotateLeft13(combined) + x) ^ y;
                auto offset =
                    static_cast<size_t>(localLane) *
                    sizeof(uint32_t);
                bytes[offset + 0] =
                    static_cast<uint8_t>(result & 0xffu);
                bytes[offset + 1] =
                    static_cast<uint8_t>(
                        (result >> 8) & 0xffu);
                bytes[offset + 2] =
                    static_cast<uint8_t>(
                        (result >> 16) & 0xffu);
                bytes[offset + 3] =
                    static_cast<uint8_t>(
                        (result >> 24) & 0xffu);
            }
        }

        std::wstring StreamingSha256(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            WorkerContentAddressedStoreRecovery const& storage,
            std::vector<std::wstring> const& pageHashes,
            uint64_t& logicalReadBytes)
        {
            auto provider =
                HashAlgorithmProvider::OpenAlgorithm(
                    HashAlgorithmNames::Sha256());
            auto hash = provider.CreateHash();
            for (uint32_t tileIndex = 0;
                 tileIndex != TileCount;
                 ++tileIndex)
            {
                auto bytes =
                    storage.ReadVerifiedCommittedPage(
                        workerRoot,
                        binding,
                        tileIndex,
                        pageHashes.at(tileIndex));
                hash.Append(
                    CryptographicBuffer::CreateFromByteArray(
                        bytes));
                logicalReadBytes += bytes.size();
            }
            auto digest = std::wstring(
                CryptographicBuffer::EncodeToHexString(
                    hash.GetValueAndReset()).c_str());
            std::transform(
                digest.begin(),
                digest.end(),
                digest.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(
                        std::towlower(ch));
                });
            return digest;
        }

        JsonObject PublishVerifiedField(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            WorkerContentAddressedStoreRecovery const& storage,
            std::vector<std::wstring> const& pageHashes,
            std::wstring const& artifactId,
            WorkerArtifactStoreConfig const& config,
            WorkerArtifactIdGenerator const& idGenerator,
            uint64_t& logicalReadBytes,
            uint64_t& logicalWriteBytes)
        {
            JsonObject begin;
            PutString(begin, L"artifact_id", artifactId);
            PutString(
                begin,
                L"artifact_kind",
                L"provable-worlds-streaming-field-v1");
            PutNumber(begin, L"expected_bytes", FieldBytes);
            PutString(
                begin,
                L"expected_sha256",
                ExpectedFieldSha256);
            WorkerExecuteBeginArtifactUpload(
                begin,
                workerRoot,
                config,
                idGenerator);
            try
            {
                for (uint32_t tileIndex = 0;
                     tileIndex != TileCount;
                     ++tileIndex)
                {
                    auto bytes =
                        storage.ReadVerifiedCommittedPage(
                            workerRoot,
                            binding,
                            tileIndex,
                            pageHashes.at(tileIndex));
                    JsonObject append;
                    PutString(
                        append,
                        L"artifact_id",
                        artifactId);
                    PutNumber(
                        append,
                        L"offset",
                        static_cast<uint64_t>(tileIndex) *
                            TileBytes);
                    PutNumber(
                        append,
                        L"byte_count",
                        bytes.size());
                    WorkerExecuteAppendArtifactChunkBinary(
                        append,
                        workerRoot,
                        bytes,
                        0.0,
                        config);
                    logicalReadBytes += bytes.size();
                    logicalWriteBytes += bytes.size();
                }
                JsonObject commit;
                PutString(commit, L"artifact_id", artifactId);
                return JsonObject::Parse(
                    hstring(
                        WorkerExecuteCommitArtifactUpload(
                            commit,
                            workerRoot,
                            config)));
            }
            catch (...)
            {
                try
                {
                    JsonObject abort;
                    PutString(
                        abort,
                        L"artifact_id",
                        artifactId);
                    WorkerExecuteAbortArtifactUpload(
                        abort,
                        workerRoot,
                        config);
                }
                catch (...)
                {
                }
                throw;
            }
        }

        JsonObject RecoveryJson(
            WorkerPersistentRecoverySnapshot const& snapshot)
        {
            JsonObject value;
            PutNumber(
                value,
                L"valid_prefix_length",
                snapshot.validPrefixLength);
            PutNumber(
                value,
                L"committed_tile_count",
                snapshot.committedPageCount);
            PutNumber(
                value,
                L"committed_bytes",
                snapshot.committedBytes);
            PutNumber(
                value,
                L"first_uncommitted_tile_index",
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
    }

    WorkerProvableWorldsStreamingError::
        WorkerProvableWorldsStreamingError(
            std::wstring codeValue,
            std::string messageValue)
        : code(std::move(codeValue)),
          message(std::move(messageValue))
    {
    }

    char const*
    WorkerProvableWorldsStreamingError::what() const noexcept
    {
        return message.c_str();
    }

    JsonObject WorkerProvableWorldsStreamingRuntime::Execute(
        std::filesystem::path const& workerRoot,
        WorkerPersistentStorageBinding const& binding,
        WorkerProvableWorldsRestartAuthorization const*
            restartAuthorization,
        std::wstring const& operation,
        std::wstring const& fieldArtifactId,
        WorkerArtifactStoreConfig const& artifactConfig,
        WorkerArtifactIdGenerator const& idGenerator) const
    {
        auto operationStarted =
            std::chrono::steady_clock::now();
        auto memoryBefore =
            CaptureMemorySample();
        if (operation != L"uninterrupted" &&
            operation != L"seed_restart" &&
            operation != L"resume_restart")
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_OPERATION_INVALID",
                "the streaming operation is unsupported");
        }
        if (binding.logicalPageBytes != TileBytes ||
            binding.maximumPageCount != TileCount ||
            binding.maximumJournalRecords != TileCount + 1 ||
            binding.maximumBytes != FieldBytes)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_STORAGE_GEOMETRY_INVALID",
                "the persistent storage binding differs from the frozen tiled geometry");
        }
        if (operation == L"resume_restart")
        {
            if (restartAuthorization == nullptr ||
                restartAuthorization->persistedReservationId !=
                    binding.reservationId ||
                restartAuthorization->persistedGeneration !=
                    binding.generation ||
                restartAuthorization->activeReservationId.empty() ||
                restartAuthorization->activeGeneration <=
                    restartAuthorization->persistedGeneration ||
                !restartAuthorization->workerBrokerBindingAgreed)
            {
                throw WorkerProvableWorldsStreamingError(
                    L"T8_RESTART_REAUTHORIZATION_INVALID",
                    "resume requires a verified fresh lease over the exact persisted storage binding");
            }
        }
        else if (restartAuthorization != nullptr)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_RESTART_REAUTHORIZATION_UNEXPECTED",
                "restart reauthorization is legal only for resume");
        }

        WorkerContentAddressedStoreRecovery storage;
        auto recoveryStarted =
            std::chrono::steady_clock::now();
        auto initial =
            storage.RecoverCommittedPrefix(
                workerRoot,
                binding);
        auto recoveryMs =
            ElapsedMilliseconds(recoveryStarted);
        if (initial.invalidTailQuarantined ||
            initial.publicationEligible ||
            initial.committedPageCount >
                TileCount ||
            initial.orderedPageSha256.size() !=
                initial.committedPageCount ||
            initial.orderedPageIndices.size() !=
                initial.committedPageCount)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_RECOVERY_PREFIX_INVALID",
                "the recovered tiled prefix is not appendable");
        }
        if (operation == L"uninterrupted" &&
            initial.committedPageCount != 0)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_UNINTERRUPTED_NAMESPACE_NOT_EMPTY",
                "an uninterrupted run requires an empty namespace");
        }
        if (operation == L"seed_restart" &&
            initial.committedPageCount != 0)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_SEED_NAMESPACE_NOT_EMPTY",
                "a restart seed requires an empty namespace");
        }
        if (operation == L"resume_restart" &&
            initial.committedPageCount !=
                StopBeforeCommitTileIndex)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_RESUME_PREFIX_INVALID",
                "resume requires the exact committed prefix ending before tile 128");
        }
        for (uint64_t pageIndex = 0;
             pageIndex != initial.orderedPageIndices.size();
             ++pageIndex)
        {
            if (initial.orderedPageIndices[pageIndex] !=
                pageIndex)
            {
                throw WorkerProvableWorldsStreamingError(
                    L"T8_RECOVERY_ORDER_INVALID",
                    "the recovered committed prefix is not contiguous and zero-based");
            }
        }

        uint64_t logicalReadBytes = 0;
        uint64_t logicalWriteBytes = 0;
        uint64_t physicalWriteBytes = 0;
        uint64_t deduplicatedTileCount = 0;
        uint64_t replayedTileCount = 0;
        uint64_t computedTileCount = 0;
        uint64_t skippedTileCount =
            initial.committedPageCount;
        uint64_t sourceInstructions = 0;
        double computeMs = 0.0;
        double commitMs = 0.0;
        double publicationMs = 0.0;
        std::array<std::vector<uint8_t>, 2>
            outputBuffers;
        for (auto& buffer : outputBuffers)
        {
            buffer.reserve(TileBytes);
        }
        auto pageHashes =
            initial.orderedPageSha256;
        auto firstTile =
            static_cast<uint32_t>(
                initial.firstUncommittedPageIndex);
        bool hasPendingCommit = false;
        uint32_t pendingTileIndex = 0;
        size_t pendingBufferIndex = 0;
        WorkerCpuGpuConvergenceExactTileResult
            capsuleIdentity;
        bool interruptionArmed = false;
        auto pipelineStarted =
            std::chrono::steady_clock::now();

        auto commitPending = [&]()
        {
            auto started =
                std::chrono::steady_clock::now();
            auto committed =
                storage.CommitVerifiedPage(
                    workerRoot,
                    binding,
                    pendingTileIndex,
                    outputBuffers[pendingBufferIndex],
                    L"RECOVERY");
            commitMs += ElapsedMilliseconds(started);
            logicalWriteBytes +=
                outputBuffers[pendingBufferIndex].size();
            if (committed.logicalReplay)
            {
                ++replayedTileCount;
            }
            else if (committed.deduplicated)
            {
                ++deduplicatedTileCount;
            }
            else
            {
                physicalWriteBytes +=
                    outputBuffers[
                        pendingBufferIndex].size();
            }
            if (pageHashes.size() != pendingTileIndex)
            {
                throw WorkerProvableWorldsStreamingError(
                    L"T8_DURABLE_CURSOR_INVALID",
                    "the durable cursor advanced outside ordered commit acknowledgment");
            }
            pageHashes.push_back(
                committed.contentSha256);
            hasPendingCommit = false;
        };

        auto lastTileInclusive =
            operation == L"seed_restart"
                ? StopBeforeCommitTileIndex
                : TileCount - 1;
        for (uint32_t tileIndex = firstTile;
             tileIndex <= lastTileInclusive;
             ++tileIndex)
        {
            auto prefetchTileIndex =
                tileIndex + 1 < TileCount
                    ? tileIndex + 1
                    : TileCount;
            (void)prefetchTileIndex;
            auto bufferIndex =
                static_cast<size_t>(tileIndex & 1u);
            auto started =
                std::chrono::steady_clock::now();
            BuildCanonicalTile(
                tileIndex,
                outputBuffers[bufferIndex]);
            capsuleIdentity =
                WorkerRunCpuGpuConvergenceExactTile(
                    tileIndex * TileLanes,
                    TileLanes,
                    InputSeed,
                    outputBuffers[bufferIndex]);
            computeMs += ElapsedMilliseconds(started);
            ++computedTileCount;
            sourceInstructions +=
                capsuleIdentity.sourceInstructions;

            if (hasPendingCommit)
            {
                commitPending();
            }
            if (operation == L"seed_restart" &&
                tileIndex ==
                    StopBeforeCommitTileIndex)
            {
                interruptionArmed = true;
                break;
            }
            pendingTileIndex = tileIndex;
            pendingBufferIndex = bufferIndex;
            hasPendingCommit = true;
        }
        if (!interruptionArmed && hasPendingCommit)
        {
            commitPending();
        }
        auto pipelineMs =
            ElapsedMilliseconds(pipelineStarted);

        if (interruptionArmed)
        {
            auto seeded =
                storage.RecoverCommittedPrefix(
                    workerRoot,
                    binding);
            if (seeded.committedPageCount !=
                    StopBeforeCommitTileIndex ||
                seeded.firstUncommittedPageIndex !=
                    StopBeforeCommitTileIndex ||
                seeded.publicationEligible)
            {
                throw WorkerProvableWorldsStreamingError(
                    L"T8_INTERRUPTION_BOUNDARY_INVALID",
                    "the seed did not stop after compute and before tile 128 commit");
            }
            JsonObject result;
            PutString(
                result,
                L"schema_version",
                L"xcp-provable-worlds-streaming-tiled-v1-operation-result");
            PutString(result, L"operation", operation);
            PutString(
                result,
                L"status",
                L"INTERRUPTION_ARMED_AFTER_COMPUTE_BEFORE_COMMIT");
            PutBool(result, L"ok", true);
            PutBool(
                result,
                L"lease_active_at_seed_boundary",
                true);
            PutNumber(
                result,
                L"computed_tile_count",
                computedTileCount);
            PutNumber(
                result,
                L"committed_tile_count",
                seeded.committedPageCount);
            PutNumber(
                result,
                L"first_uncommitted_tile_index",
                seeded.firstUncommittedPageIndex);
            PutNumber(
                result,
                L"source_instructions",
                sourceInstructions);
            PutMetric(result, L"recovery_ms", recoveryMs);
            PutMetric(result, L"compute_ms", computeMs);
            PutMetric(result, L"commit_ms", commitMs);
            PutMetric(result, L"pipeline_ms", pipelineMs);
            result.SetNamedValue(
                L"recovery",
                RecoveryJson(seeded));
            return result;
        }

        if (pageHashes.size() != TileCount)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_COMMITTED_TILE_COUNT_INVALID",
                "the streaming run did not commit exactly 256 ordered tiles");
        }
        for (auto& buffer : outputBuffers)
        {
            buffer.clear();
            buffer.shrink_to_fit();
        }
        auto rootCommitStarted =
            std::chrono::steady_clock::now();
        auto rootCommit =
            storage.CommitVerifiedRoot(
                workerRoot,
                binding,
                pageHashes);
        commitMs +=
            ElapsedMilliseconds(rootCommitStarted);
        auto finalRecovery =
            storage.RecoverCommittedPrefix(
                workerRoot,
                binding);
        if (!finalRecovery.publicationEligible ||
            finalRecovery.committedPageCount !=
                TileCount ||
            finalRecovery.committedRootSha256 !=
                rootCommit.rootSha256)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_ORDERED_ROOT_INVALID",
                "the terminal ordered root did not verify");
        }

        auto fieldSha256 =
            StreamingSha256(
                workerRoot,
                binding,
                storage,
                pageHashes,
                logicalReadBytes);
        if (fieldSha256 != ExpectedFieldSha256)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_FINAL_FIELD_SHA256_MISMATCH",
                "the verified ordered field differs from the frozen oracle");
        }

        auto publishStarted =
            std::chrono::steady_clock::now();
        auto artifact =
            PublishVerifiedField(
                workerRoot,
                binding,
                storage,
                pageHashes,
                fieldArtifactId,
                artifactConfig,
                idGenerator,
                logicalReadBytes,
                logicalWriteBytes);
        publicationMs =
            ElapsedMilliseconds(publishStarted);
        auto artifactSha256 =
            std::wstring(
                artifact.GetNamedString(
                    L"sha256").c_str());
        if (artifactSha256 != fieldSha256)
        {
            throw WorkerProvableWorldsStreamingError(
                L"T8_ARTIFACT_SHA256_MISMATCH",
                "the published artifact differs from the verified ordered field");
        }
        auto memoryAfter =
            CaptureMemorySample();
        auto peakRamWorkingSetBytes =
            (std::max)(
                memoryBefore.peakWorkingSetBytes,
                memoryAfter.peakWorkingSetBytes);
        auto namespaceRoot =
            workerRoot.parent_path() /
            L"xcompute-storage-v1" /
            L"namespaces" /
            binding.namespaceId;
        auto journalBytes =
            DirectoryBytes(
                namespaceRoot / L"journal");
        auto namespaceBytes =
            DirectoryBytes(namespaceRoot);
        auto metadataBytes =
            namespaceBytes >= journalBytes
                ? namespaceBytes - journalBytes
                : 0;
        auto artifactDeduplicated =
            artifact.GetNamedBoolean(
                L"deduplicated");
        auto physicalArtifactWriteBytes =
            FieldBytes +
            (artifactDeduplicated ? 0 : FieldBytes);
        auto physicalBytesRead =
            logicalReadBytes +
            (initial.committedPageCount +
                finalRecovery.committedPageCount) *
                TileBytes;
        auto physicalBytesWritten =
            physicalWriteBytes +
            physicalArtifactWriteBytes +
            metadataBytes +
            journalBytes;
        auto operationPeakSsdBytes =
            physicalWriteBytes +
            physicalArtifactWriteBytes +
            metadataBytes +
            journalBytes;
        auto deduplicationBytes =
            deduplicatedTileCount * TileBytes +
            (artifactDeduplicated ? FieldBytes : 0);
        auto endToEndMs =
            ElapsedMilliseconds(operationStarted);
        auto throughputBytesPerSecond =
            endToEndMs > 0.0
                ? static_cast<double>(FieldBytes) /
                    (endToEndMs / 1000.0)
                : 0.0;
        auto writeAmplificationRatio =
            logicalWriteBytes > 0
                ? static_cast<double>(
                    physicalBytesWritten) /
                    static_cast<double>(
                        logicalWriteBytes)
                : 0.0;

        JsonObject result;
        PutString(
            result,
            L"schema_version",
            L"xcp-provable-worlds-streaming-tiled-v1-operation-result");
        PutString(result, L"operation", operation);
        PutString(
            result,
            L"status",
            L"OPERATION_PASS_NOT_AGGREGATE_T8");
        PutBool(result, L"ok", true);
        PutString(
            result,
            L"contract_id",
            ContractId);
        PutString(result, L"program_id", ProgramId);
        PutString(
            result,
            L"program_sha256",
            ProgramSha256);
        PutString(result, L"profile_id", ProfileId);
        PutString(
            result,
            L"profile_contract_sha256",
            ProfileContractSha256);
        PutString(
            result,
            L"bound_input_sha256",
            BoundInputSha256);
        PutString(
            result,
            L"bound_input_mix64",
            BoundInputMix64);
        PutString(
            result,
            L"field_sha256",
            fieldSha256);
        PutString(
            result,
            L"ordered_root_sha256",
            rootCommit.rootSha256);
        PutString(
            result,
            L"field_artifact_id",
            fieldArtifactId);
        PutString(
            result,
            L"field_artifact_blob_handle",
            std::wstring(
                artifact.GetNamedString(
                    L"blob_handle").c_str()));
        PutNumber(
            result,
            L"tile_count",
            TileCount);
        PutNumber(
            result,
            L"computed_tile_count",
            computedTileCount);
        PutNumber(
            result,
            L"skipped_committed_tile_count",
            skippedTileCount);
        PutNumber(
            result,
            L"exact_tile_comparison_count",
            computedTileCount);
        PutNumber(
            result,
            L"source_instructions",
            sourceInstructions);
        PutNumber(
            result,
            L"logical_read_bytes",
            logicalReadBytes);
        PutNumber(
            result,
            L"logical_input_bytes",
            sizeof(InputSeed));
        PutNumber(
            result,
            L"logical_output_bytes",
            FieldBytes);
        PutNumber(
            result,
            L"logical_write_bytes",
            logicalWriteBytes);
        PutNumber(
            result,
            L"physical_page_write_bytes",
            physicalWriteBytes);
        PutNumber(
            result,
            L"physical_bytes_read",
            physicalBytesRead);
        PutNumber(
            result,
            L"physical_bytes_written",
            physicalBytesWritten);
        PutNumber(
            result,
            L"metadata_bytes",
            metadataBytes);
        PutNumber(
            result,
            L"journal_bytes",
            journalBytes);
        PutNumber(
            result,
            L"peak_ram_working_set_bytes",
            peakRamWorkingSetBytes);
        PutNumber(
            result,
            L"operation_peak_ssd_bytes",
            operationPeakSsdBytes);
        PutNumber(
            result,
            L"resumed_units",
            operation == L"resume_restart"
                ? TileCount -
                    initial.committedPageCount
                : 0);
        PutNumber(
            result,
            L"deduplicated_tile_count",
            deduplicatedTileCount);
        PutNumber(
            result,
            L"replayed_tile_count",
            replayedTileCount);
        PutNumber(
            result,
            L"maximum_simultaneous_buffers",
            MaximumSimultaneousBuffers);
        PutNumber(
            result,
            L"maximum_buffer_bytes",
            TileBytes);
        PutBool(
            result,
            L"output_double_buffered",
            true);
        PutString(
            result,
            L"schedule",
            L"prefetch N+1 -> compute N -> commit N-1");
        PutMetric(result, L"recovery_ms", recoveryMs);
        PutMetric(result, L"prefetch_wait_ms", 0.0);
        PutMetric(result, L"compute_ms", computeMs);
        PutMetric(result, L"commit_ms", commitMs);
        PutMetric(result, L"commit_wait_ms", commitMs);
        PutMetric(
            result,
            L"publication_ms",
            publicationMs);
        PutMetric(result, L"pipeline_ms", pipelineMs);
        PutMetric(result, L"end_to_end_ms", endToEndMs);
        PutMetric(
            result,
            L"overlap_ratio_report_only",
            0.0);
        PutMetric(
            result,
            L"throughput_bytes_per_second_report_only",
            throughputBytesPerSecond);
        PutMetric(
            result,
            L"write_amplification_ratio_report_only",
            writeAmplificationRatio);
        PutNumber(
            result,
            L"deduplication_bytes",
            deduplicationBytes);
        PutString(
            result,
            L"capsule_package_full_name",
            capsuleIdentity.packageFullName);
        PutString(
            result,
            L"capsule_package_version",
            capsuleIdentity.packageVersion);
        PutString(
            result,
            L"capsule_module_path",
            capsuleIdentity.modulePath);
        PutString(
            result,
            L"capsule_module_sha256",
            capsuleIdentity.moduleSha256);
        result.SetNamedValue(
            L"recovery",
            RecoveryJson(finalRecovery));
        JsonObject subgates;
        PutString(
            subgates,
            L"t8_streaming_operation",
            L"PASS_NOT_AGGREGATE_ADMISSION");
        PutString(
            subgates,
            L"t9_product_admission",
            L"NOT_EVALUATED_REQUIRES_COMPLETE_T8_AND_REGRESSIONS");
        result.SetNamedValue(L"subgates", subgates);
        return result;
    }

    std::wstring WorkerProvableWorldsStreamingCapabilityJson()
    {
        JsonObject value;
        PutString(
            value,
            L"schema_version",
            L"xcp-provable-worlds-streaming-tiled-v1-capability");
        PutString(
            value,
            L"delivery_train",
            WorkerProvableWorldsStreamingTrainId);
        PutString(
            value,
            L"implementation_status",
            L"IMPLEMENTED_LOCAL_PACKAGE_PENDING");
        PutString(
            value,
            L"command",
            L"run_provable_worlds_streaming_tiled_v1");
        PutString(
            value,
            L"cpu_backend",
            L"signed_cpu_capsule_exact_tile_adapter");
        PutString(
            value,
            L"shared_cas_owner",
            L"WorkerArtifactStore");
        PutString(
            value,
            L"journal_owner",
            L"WorkerContentAddressedStoreRecovery");
        PutNumber(value, L"tile_width", GridWidth);
        PutNumber(value, L"tile_height", TileHeight);
        PutNumber(value, L"tile_depth", 1);
        PutNumber(value, L"tile_lanes", TileLanes);
        PutNumber(value, L"tile_bytes", TileBytes);
        PutNumber(value, L"tile_count", TileCount);
        PutNumber(
            value,
            L"maximum_simultaneous_buffers",
            MaximumSimultaneousBuffers);
        PutBool(value, L"output_double_buffered", true);
        PutBool(value, L"second_backend", false);
        PutBool(value, L"second_artifact_system", false);
        PutBool(value, L"runtime_native_codegen", false);
        PutBool(value, L"runtime_shader_compilation", false);
        PutString(
            value,
            L"t8_status",
            L"PACKAGE_AND_LIVE_MEASUREMENT_PENDING");
        PutString(
            value,
            L"t9_status",
            L"CLOSED_PENDING_T8_AND_REGRESSIONS");
        return std::wstring(value.Stringify().c_str());
    }
}
