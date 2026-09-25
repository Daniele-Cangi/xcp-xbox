#include "pch.h"
#include "WorkerProcessTopologyBrokerRuntime.h"

#include "WorkerProcessTopologyBrokerProtocol.h"
#include "WorkerProcessTopologyBrokerStorageRuntime.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::System;
using namespace Windows::System::Diagnostics;

namespace winrt::XComputeTopologyBroker::implementation
{
    namespace
    {
        constexpr size_t HashChunkBytes = 64ull * 1024ull;
        constexpr uint64_t FnvOffsetBasis =
            14695981039346656037ull;
        constexpr uint64_t FnvPrime =
            1099511628211ull;

        std::wstring Hex64(uint64_t value)
        {
            std::wostringstream out;
            out << std::hex << std::setfill(L'0')
                << std::setw(16) << value;
            return out.str();
        }

        std::wstring Lowercase(std::wstring value)
        {
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

        uint64_t Marker(
            uint64_t seed,
            uint64_t generation,
            uint64_t pageIndex)
        {
            uint64_t value =
                seed ^
                (generation * 0xd6e8feb86659fd93ull) ^
                pageIndex;
            value += 0x9e3779b97f4a7c15ull;
            value =
                (value ^ (value >> 30)) *
                0xbf58476d1ce4e5b9ull;
            value =
                (value ^ (value >> 27)) *
                0x94d049bb133111ebull;
            return value ^ (value >> 31);
        }

        class DigestAccumulator final
        {
        public:
            DigestAccumulator()
                : hash_(
                    HashAlgorithmProvider::OpenAlgorithm(
                        HashAlgorithmNames::Sha256()).CreateHash())
            {
                bytes_.reserve(HashChunkBytes);
            }

            void AppendMarker(uint64_t marker)
            {
                for (
                    unsigned int shift = 0;
                    shift != 64;
                    shift += 8)
                {
                    auto byte = static_cast<uint8_t>(
                        (marker >> shift) & 0xffu);
                    bytes_.push_back(byte);
                    fnv64_ ^= byte;
                    fnv64_ *= FnvPrime;
                }
                if (bytes_.size() >= HashChunkBytes)
                {
                    Flush();
                }
            }

            BrokerMemoryDigest Finish()
            {
                Flush();
                auto encoded =
                    CryptographicBuffer::EncodeToHexString(
                        hash_.GetValueAndReset());
                return BrokerMemoryDigest{
                    fnv64_,
                    hstring(
                        Lowercase(
                            std::wstring(encoded.c_str()))) };
            }

        private:
            void Flush()
            {
                if (bytes_.empty())
                {
                    return;
                }
                hash_.Append(
                    CryptographicBuffer::CreateFromByteArray(
                        bytes_));
                bytes_.clear();
            }

            CryptographicHash hash_{ nullptr };
            std::vector<uint8_t> bytes_;
            uint64_t fnv64_ = FnvOffsetBasis;
        };

        std::wstring PackageVersionString(
            PackageVersion const& version)
        {
            std::wostringstream out;
            out << version.Major << L'.'
                << version.Minor << L'.'
                << version.Build << L'.'
                << version.Revision;
            return out.str();
        }

        uint64_t SafetyReserve(uint64_t limit)
        {
            constexpr uint64_t minimum =
                16ull * 1024ull * 1024ull;
            constexpr uint64_t maximum =
                256ull * 1024ull * 1024ull;
            return (std::max)(
                minimum,
                (std::min)(maximum, limit / 16ull));
        }
    }

    WorkerProcessTopologyBrokerRuntime::
        WorkerProcessTopologyBrokerRuntime()
        : lifecycleJournal_(L"broker"),
          activationId_(lifecycleJournal_.ActivationIdentity()),
          activatedAt_(std::chrono::steady_clock::now()),
          heartbeatStatus_([this](uint64_t sequence)
          {
              lifecycleJournal_.Record(L"heartbeat", sequence);
          })
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        pageSize_ = info.dwPageSize;
    }

    WorkerProcessTopologyBrokerRuntime::
        ~WorkerProcessTopologyBrokerRuntime()
    {
        lifecycleJournal_.Record(L"terminated");
        coordinatorRuntime_.ReleaseAll(L"broker_terminated");
        Release(L"runtime_destructor");
    }

    hstring const&
        WorkerProcessTopologyBrokerRuntime::ActivationId()
            const noexcept
    {
        return activationId_;
    }

    JsonObject WorkerProcessTopologyBrokerRuntime::ExecuteCommand(
        hstring const& command,
        JsonObject const& request)
    {
        std::lock_guard guard(mutex_);
        if (command == L"describe")
        {
            return DescribeLocked();
        }
        if (command == L"phase_status")
        {
            return PhaseStatusLocked();
        }
        if (command == L"phase_begin")
        {
            return PhaseBeginLocked(request);
        }
        if (command == L"phase_complete")
        {
            return PhaseCompleteLocked(request);
        }
        if (command == L"allocate_touch")
        {
            return AllocateTouchLocked(request);
        }
        if (command == L"heartbeat")
        {
            return HeartbeatLocked(request);
        }
        if (command == L"release")
        {
            return ReleaseLocked(L"explicit_release");
        }
        if (command == L"payload")
        {
            return Payload(request);
        }
        if (command == L"storage_exchange")
        {
            return WorkerProcessTopologyBrokerStorageRuntime::Exchange(
                phaseRuntime_, request);
        }
        if (command == L"storage_cleanup_status")
        {
            return WorkerProcessTopologyBrokerStorageRuntime::CleanupStatus(
                phaseRuntime_, request);
        }
        if (command == L"coordinator_describe")
        {
            return coordinatorRuntime_.Describe();
        }
        if (command == L"reservation_acquire")
        {
            return coordinatorRuntime_.Acquire(request);
        }
        if (command == L"reservation_status")
        {
            return coordinatorRuntime_.Status(request);
        }
        if (command == L"reservation_release")
        {
            return coordinatorRuntime_.Release(request);
        }
        FailBrokerRequest(
            L"UNKNOWN_COMMAND",
            L"supported commands are describe, phase_status, phase_begin, phase_complete, allocate_touch, heartbeat, release, payload, storage_exchange, storage_cleanup_status, coordinator_describe, reservation_acquire, reservation_status, reservation_release");
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::DescribeLocked() const
    {
        JsonObject result;
        PutBrokerString(
            result,
            L"runtime_class",
            BrokerEntryPointName);
        PutBrokerString(
            result,
            L"request_schema_version",
            BrokerRequestSchema);
        PutBrokerString(
            result,
            L"phase_status_schema",
            BrokerPhaseStatusSchema);
        PutBrokerString(
            result,
            L"phase_control_schema",
            BrokerPhaseControlSchema);
        PutBrokerBoolean(
            result,
            L"autonomous_heartbeat",
            true);
        PutBrokerBoolean(
            result,
            L"phase_status_read_only",
            true);
        PutBrokerBoolean(
            result,
            L"phase_assignment_supported",
            true);
        PutBrokerBoolean(
            result,
            L"full_t2_phase_instrumentation_complete",
            true);
        PutBrokerBoolean(
            result,
            L"phase_evidence_acceptance_authority",
            false);
        PutBrokerBoolean(
            result,
            L"phase_classification_authority",
            false);
        PutBrokerBoolean(
            result,
            L"heartbeat_mutates_by_default",
            false);
        PutBrokerString(
            result,
            L"process_role",
            L"out_of_process_app_service_broker");
        PutBrokerString(
            result,
            L"app_service_name",
            BrokerContractName);
        PutBrokerString(
            result,
            L"resource_group",
            BrokerContractName);
        PutBrokerBoolean(
            result,
            L"requires_dedicated_resource_group",
            true);
        PutBrokerString(
            result,
            L"primary_digest",
            L"sha256");
        PutBrokerString(
            result,
            L"digest_scope",
            BrokerDigestScope);
        PutBrokerNumber(
            result,
            L"maximum_allocation_bytes",
            BrokerMaximumAllocationBytes);
        PutBrokerNumber(
            result,
            L"maximum_request_utf8_bytes",
            BrokerMaximumRequestUtf8Bytes);
        PutBrokerNumber(
            result,
            L"maximum_payload_utf8_bytes",
            BrokerMaximumPayloadUtf8Bytes);

        JsonArray commands;
        for (auto const* name : {
            L"describe",
            L"phase_status",
            L"phase_begin",
            L"phase_complete",
            L"allocate_touch",
            L"heartbeat",
            L"release",
            L"payload",
            L"storage_exchange",
            L"storage_cleanup_status",
            L"coordinator_describe",
            L"reservation_acquire",
            L"reservation_status",
            L"reservation_release" })
        {
            commands.Append(
                JsonValue::CreateStringValue(name));
        }
        result.SetNamedValue(L"commands", commands);

        JsonObject authority;
        PutBrokerBoolean(authority, L"semantic", false);
        PutBrokerBoolean(authority, L"storage", false);
        PutBrokerBoolean(
            authority,
            L"artifact_publication",
            false);
        PutBrokerBoolean(authority, L"network", false);
        PutBrokerBoolean(
            authority,
            L"storage_access",
            false);
        result.SetNamedValue(L"authority", authority);
        result.SetNamedValue(
            L"phase_status",
            PhaseStatusLocked());
        result.SetNamedValue(L"metrics", MetricsLocked());
        result.SetNamedValue(
            L"lifecycle_journal",
            lifecycleJournal_.Snapshot());
        result.SetNamedValue(
            L"allocation",
            AllocationLocked());
        result.SetNamedValue(
            L"persistent_compute_coordinator",
            coordinatorRuntime_.Describe());
        return result;
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::PhaseStatusLocked() const
    {
        return phaseRuntime_.Status(
            heartbeatStatus_.Snapshot());
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::PhaseBeginLocked(
            JsonObject const& request)
    {
        return phaseRuntime_.Begin(
            request,
            heartbeatStatus_.Snapshot());
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::PhaseCompleteLocked(
            JsonObject const& request)
    {
        return phaseRuntime_.Complete(
            request,
            heartbeatStatus_.Snapshot());
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::AllocateTouchLocked(
            JsonObject const& request)
    {
        if (allocation_)
        {
            FailBrokerRequest(
                L"ALLOCATION_ALREADY_ACTIVE",
                L"release the active allocation before allocating again");
        }
        auto bytes = ReadBrokerIntegral(
            request,
            L"bytes",
            0,
            true);
        auto seed = ReadBrokerIntegral(
            request,
            L"seed",
            BrokerDefaultSeed,
            false);
        if (
            bytes == 0 ||
            pageSize_ == 0 ||
            bytes % pageSize_ != 0)
        {
            FailBrokerRequest(
                L"ALLOCATION_NOT_PAGE_ALIGNED",
                L"bytes must be a positive multiple of the reported page_size_bytes");
        }
        if (
            bytes > BrokerMaximumAllocationBytes ||
            bytes > static_cast<uint64_t>(
                (std::numeric_limits<SIZE_T>::max)()))
        {
            FailBrokerRequest(
                L"ALLOCATION_ABSOLUTE_LIMIT",
                L"requested allocation exceeds the broker absolute bound");
        }

        auto usage = MemoryManager::AppMemoryUsage();
        auto limit = MemoryManager::AppMemoryUsageLimit();
        auto reserve = SafetyReserve(limit);
        if (
            limit == 0 ||
            usage >= limit ||
            bytes > limit - usage ||
            reserve > limit - usage - bytes)
        {
            FailBrokerRequest(
                L"ALLOCATION_HEADROOM_LIMIT",
                L"requested allocation would cross the current app-memory limit safety reserve");
        }

        void* memory = VirtualAllocFromApp(
            nullptr,
            static_cast<SIZE_T>(bytes),
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (!memory)
        {
            FailBrokerRequest(
                L"ALLOCATION_FAILED",
                L"VirtualAllocFromApp could not reserve and commit the requested region");
        }

        allocation_ = memory;
        allocationBytes_ = bytes;
        pageCount_ = bytes / pageSize_;
        allocationSeed_ = seed;
        allocationGeneration_ = 1;
        try
        {
            auto digest =
                TouchAndDigestLocked(allocationGeneration_);
            allocationDigest_ = digest.fnv64;
            allocationDigestSha256_ = digest.sha256;
            if (CommittedBytesLocked() != allocationBytes_)
            {
                ReleaseAllocationLocked(
                    L"commit_validation_failed");
                FailBrokerRequest(
                    L"COMMIT_VALIDATION_FAILED",
                    L"VirtualQuery did not report the full requested range as MEM_COMMIT");
            }
        }
        catch (...)
        {
            ReleaseAllocationLocked(
                L"allocation_initialization_failed");
            throw;
        }

        JsonObject result;
        result.SetNamedValue(
            L"allocation",
            AllocationLocked());
        result.SetNamedValue(L"metrics", MetricsLocked());
        return result;
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::HeartbeatLocked(
            JsonObject const& request)
    {
        bool mutate = ReadBrokerBoolean(
            request,
            L"mutate",
            false);
        bool retentionVerified = false;
        bool mutated = false;
        if (allocation_)
        {
            auto retainedDigest = VerifyAndDigestLocked();
            if (
                retainedDigest.fnv64 != allocationDigest_ ||
                retainedDigest.sha256 !=
                    allocationDigestSha256_)
            {
                FailBrokerRequest(
                    L"DIGEST_MISMATCH",
                    L"the previous allocation generation no longer matches its canonical digest");
            }
            retentionVerified = true;
            if (mutate)
            {
                ++allocationGeneration_;
                auto mutatedDigest =
                    TouchAndDigestLocked(
                        allocationGeneration_);
                allocationDigest_ = mutatedDigest.fnv64;
                allocationDigestSha256_ =
                    mutatedDigest.sha256;
                mutated = true;
            }
        }
        ++heartbeatCount_;

        JsonObject result;
        PutBrokerNumber(
            result,
            L"heartbeat_count",
            heartbeatCount_);
        PutBrokerBoolean(
            result,
            L"retention_verified",
            retentionVerified);
        PutBrokerBoolean(result, L"mutated", mutated);
        result.SetNamedValue(
            L"phase_status",
            PhaseStatusLocked());
        result.SetNamedValue(
            L"allocation",
            AllocationLocked());
        result.SetNamedValue(L"metrics", MetricsLocked());
        return result;
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::ReleaseLocked(
            std::wstring_view reason)
    {
        bool hadAllocation = allocation_ != nullptr;
        bool released = ReleaseAllocationLocked(reason);
        JsonObject result;
        PutBrokerBoolean(
            result,
            L"had_allocation",
            hadAllocation);
        PutBrokerBoolean(result, L"released", released);
        PutBrokerBoolean(
            result,
            L"idempotent_success",
            !hadAllocation || released);
        PutBrokerNumber(
            result,
            L"last_released_bytes",
            lastReleasedBytes_);
        PutBrokerString(
            result,
            L"release_reason",
            lastReleaseReason_);
        result.SetNamedValue(L"metrics", MetricsLocked());
        return result;
    }

    JsonObject WorkerProcessTopologyBrokerRuntime::Payload(
        JsonObject const& request) const
    {
        if (!request.HasKey(L"payload"))
        {
            FailBrokerRequest(
                L"MISSING_ARGUMENT",
                L"payload command requires a payload string");
        }
        hstring payload;
        try
        {
            payload = request.GetNamedString(L"payload");
        }
        catch (...)
        {
            FailBrokerRequest(
                L"INVALID_ARGUMENT_TYPE",
                L"payload must be a JSON string");
        }
        auto bytes = to_string(payload);
        if (bytes.size() > BrokerMaximumPayloadUtf8Bytes)
        {
            FailBrokerRequest(
                L"PAYLOAD_TOO_LARGE",
                L"payload exceeds the broker UTF-8 transport bound");
        }

        std::vector<uint8_t> byteVector(
            bytes.begin(),
            bytes.end());
        auto provider =
            HashAlgorithmProvider::OpenAlgorithm(
                HashAlgorithmNames::Sha256());
        auto sha256 = Lowercase(
            std::wstring(
                CryptographicBuffer::EncodeToHexString(
                    provider.HashData(
                        CryptographicBuffer::
                            CreateFromByteArray(
                                byteVector))).c_str()));
        uint64_t fnv = FnvOffsetBasis;
        for (auto byte : byteVector)
        {
            fnv ^= byte;
            fnv *= FnvPrime;
        }

        JsonObject result;
        PutBrokerNumber(
            result,
            L"payload_utf8_bytes",
            byteVector.size());
        PutBrokerString(
            result,
            L"digest_sha256",
            sha256);
        PutBrokerString(
            result,
            L"digest_fnv64",
            Hex64(fnv));
        PutBrokerBoolean(
            result,
            L"echoed",
            ReadBrokerBoolean(
                request,
                L"echo",
                false));
        if (ReadBrokerBoolean(request, L"echo", false))
        {
            PutBrokerString(
                result,
                L"payload",
                std::wstring_view(payload));
        }
        return result;
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::MetricsLocked() const
    {
        JsonObject metrics;
        PutBrokerNumber(
            metrics,
            L"process_id",
            GetCurrentProcessId());
        PutBrokerNumber(
            metrics,
            L"thread_id",
            GetCurrentThreadId());
        PutBrokerNumber(
            metrics,
            L"uptime_ms",
            static_cast<uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        activatedAt_).count()));
        PutBrokerNumber(
            metrics,
            L"app_memory_usage_bytes",
            MemoryManager::AppMemoryUsage());
        PutBrokerNumber(
            metrics,
            L"app_memory_usage_limit_bytes",
            MemoryManager::AppMemoryUsageLimit());
        PutBrokerNumber(
            metrics,
            L"app_memory_usage_level",
            static_cast<uint64_t>(
                MemoryManager::AppMemoryUsageLevel()));
        PutBrokerNumber(
            metrics,
            L"memory_limit_event_count",
            limitEventCount_);
        PutBrokerNumber(
            metrics,
            L"memory_limit_forced_release_count",
            limitForcedReleaseCount_);
        PutBrokerNumber(
            metrics,
            L"last_old_limit_bytes",
            lastOldLimitBytes_);
        PutBrokerNumber(
            metrics,
            L"last_new_limit_bytes",
            lastNewLimitBytes_);

        try
        {
            auto packageId = Package::Current().Id();
            JsonObject package;
            PutBrokerString(
                package,
                L"name",
                std::wstring_view(packageId.Name()));
            PutBrokerString(
                package,
                L"family_name",
                std::wstring_view(
                    packageId.FamilyName()));
            PutBrokerString(
                package,
                L"full_name",
                std::wstring_view(packageId.FullName()));
            PutBrokerString(
                package,
                L"version",
                PackageVersionString(
                    packageId.Version()));
            PutBrokerString(
                package,
                L"architecture",
                L"x64");
            metrics.SetNamedValue(
                L"package_identity",
                package);
        }
        catch (...)
        {
            PutBrokerString(
                metrics,
                L"package_identity_status",
                L"unavailable");
        }

        try
        {
            auto report =
                MemoryManager::GetAppMemoryReport();
            JsonObject commit;
            PutBrokerNumber(
                commit,
                L"private_commit_usage_bytes",
                report.PrivateCommitUsage());
            PutBrokerNumber(
                commit,
                L"total_commit_usage_bytes",
                report.TotalCommitUsage());
            metrics.SetNamedValue(L"app_commit", commit);

            auto processReport =
                MemoryManager::GetProcessMemoryReport();
            JsonObject workingSet;
            PutBrokerNumber(
                workingSet,
                L"private_bytes",
                processReport.PrivateWorkingSetUsage());
            PutBrokerNumber(
                workingSet,
                L"total_bytes",
                processReport.TotalWorkingSetUsage());
            metrics.SetNamedValue(
                L"process_working_set",
                workingSet);
        }
        catch (...)
        {
            PutBrokerString(
                metrics,
                L"memory_report_status",
                L"unavailable");
        }

        try
        {
            auto diagnostic =
                ProcessDiagnosticInfo::GetForCurrentProcess();
            auto memory =
                diagnostic.MemoryUsage().GetReport();
            JsonObject processMemory;
            PutBrokerNumber(
                processMemory,
                L"working_set_bytes",
                memory.WorkingSetSizeInBytes());
            PutBrokerNumber(
                processMemory,
                L"private_page_bytes",
                memory.PrivatePageCount());
            PutBrokerNumber(
                processMemory,
                L"virtual_memory_bytes",
                memory.VirtualMemorySizeInBytes());
            PutBrokerNumber(
                processMemory,
                L"page_file_bytes",
                memory.PageFileSizeInBytes());
            PutBrokerNumber(
                processMemory,
                L"page_fault_count",
                memory.PageFaultCount());
            metrics.SetNamedValue(
                L"process_diagnostics_memory",
                processMemory);

            auto cpu = diagnostic.CpuUsage().GetReport();
            JsonObject processCpu;
            PutBrokerNumber(
                processCpu,
                L"user_time_100ns",
                static_cast<uint64_t>(
                    cpu.UserTime().count()));
            PutBrokerNumber(
                processCpu,
                L"kernel_time_100ns",
                static_cast<uint64_t>(
                    cpu.KernelTime().count()));
            metrics.SetNamedValue(
                L"process_diagnostics_cpu",
                processCpu);
        }
        catch (...)
        {
            PutBrokerString(
                metrics,
                L"process_diagnostics_status",
                L"unavailable");
        }
        return metrics;
    }

    JsonObject
        WorkerProcessTopologyBrokerRuntime::AllocationLocked()
            const
    {
        JsonObject allocation;
        PutBrokerBoolean(
            allocation,
            L"active",
            allocation_ != nullptr);
        PutBrokerNumber(
            allocation,
            L"requested_bytes",
            allocationBytes_);
        PutBrokerNumber(
            allocation,
            L"committed_bytes",
            allocation_
                ? CommittedBytesLocked()
                : 0);
        PutBrokerNumber(
            allocation,
            L"page_size_bytes",
            pageSize_);
        PutBrokerNumber(
            allocation,
            L"page_count",
            pageCount_);
        PutBrokerNumber(
            allocation,
            L"generation",
            allocationGeneration_);
        PutBrokerString(
            allocation,
            L"digest_algorithm",
            L"sha256");
        PutBrokerString(
            allocation,
            L"digest_scope",
            BrokerDigestScope);
        PutBrokerString(
            allocation,
            L"digest_sha256",
            std::wstring_view(
                allocationDigestSha256_));
        PutBrokerString(
            allocation,
            L"digest_fnv64",
            Hex64(allocationDigest_));
        return allocation;
    }

    BrokerMemoryDigest
        WorkerProcessTopologyBrokerRuntime::TouchAndDigestLocked(
            uint64_t generation)
    {
        DigestAccumulator digest;
        auto base =
            static_cast<volatile uint8_t*>(allocation_);
        for (
            uint64_t pageIndex = 0;
            pageIndex < pageCount_;
            ++pageIndex)
        {
            auto marker = Marker(
                allocationSeed_,
                generation,
                pageIndex);
            auto page =
                base + pageIndex * pageSize_;
            for (
                unsigned int byteIndex = 0;
                byteIndex != 8;
                ++byteIndex)
            {
                page[byteIndex] =
                    static_cast<uint8_t>(
                        (marker >>
                            (byteIndex * 8)) &
                        0xffu);
            }
            page[pageSize_ - 1] =
                static_cast<uint8_t>(
                    (marker >> 56) & 0xffu);
            uint64_t observed = 0;
            for (
                unsigned int byteIndex = 0;
                byteIndex != 8;
                ++byteIndex)
            {
                observed |=
                    static_cast<uint64_t>(
                        page[byteIndex]) <<
                    (byteIndex * 8);
            }
            if (
                observed != marker ||
                page[pageSize_ - 1] !=
                    static_cast<uint8_t>(
                        (marker >> 56) & 0xffu))
            {
                FailBrokerRequest(
                    L"MEMORY_TOUCH_MISMATCH",
                    L"a committed page did not retain its deterministic marker");
            }
            digest.AppendMarker(observed);
        }
        return digest.Finish();
    }

    BrokerMemoryDigest
        WorkerProcessTopologyBrokerRuntime::VerifyAndDigestLocked()
            const
    {
        DigestAccumulator digest;
        auto base =
            static_cast<volatile uint8_t const*>(
                allocation_);
        for (
            uint64_t pageIndex = 0;
            pageIndex < pageCount_;
            ++pageIndex)
        {
            auto marker = Marker(
                allocationSeed_,
                allocationGeneration_,
                pageIndex);
            auto page =
                base + pageIndex * pageSize_;
            uint64_t observed = 0;
            for (
                unsigned int byteIndex = 0;
                byteIndex != 8;
                ++byteIndex)
            {
                observed |=
                    static_cast<uint64_t>(
                        page[byteIndex]) <<
                    (byteIndex * 8);
            }
            if (
                observed != marker ||
                page[pageSize_ - 1] !=
                    static_cast<uint8_t>(
                        (marker >> 56) & 0xffu))
            {
                FailBrokerRequest(
                    L"MEMORY_VERIFY_MISMATCH",
                    L"an active page marker changed between heartbeats");
            }
            digest.AppendMarker(observed);
        }
        return digest.Finish();
    }

    uint64_t
        WorkerProcessTopologyBrokerRuntime::CommittedBytesLocked()
            const
    {
        if (!allocation_ || allocationBytes_ == 0)
        {
            return 0;
        }
        auto cursor =
            static_cast<uint8_t const*>(allocation_);
        auto end = cursor + allocationBytes_;
        uint64_t committed = 0;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (
                VirtualQuery(
                    cursor,
                    &region,
                    sizeof(region)) == 0 ||
                region.RegionSize == 0)
            {
                break;
            }
            auto regionStart =
                static_cast<uint8_t const*>(
                    region.BaseAddress);
            auto regionEnd =
                regionStart + region.RegionSize;
            auto overlapStart =
                (std::max)(cursor, regionStart);
            auto overlapEnd =
                (std::min)(end, regionEnd);
            if (
                region.State == MEM_COMMIT &&
                overlapEnd > overlapStart)
            {
                committed +=
                    static_cast<uint64_t>(
                        overlapEnd - overlapStart);
            }
            if (regionEnd <= cursor)
            {
                break;
            }
            cursor = regionEnd;
        }
        return committed;
    }

    bool WorkerProcessTopologyBrokerRuntime::
        ReleaseAllocationLocked(
            std::wstring_view reason) noexcept
    {
        if (!allocation_)
        {
            lastReleaseReason_ = reason;
            return false;
        }
        auto memory = allocation_;
        auto bytes = allocationBytes_;
        if (!VirtualFree(memory, 0, MEM_RELEASE))
        {
            lastReleaseReason_ =
                L"virtual_free_failed";
            return false;
        }
        allocation_ = nullptr;
        allocationBytes_ = 0;
        pageCount_ = 0;
        allocationSeed_ = 0;
        allocationGeneration_ = 0;
        allocationDigest_ = 0;
        allocationDigestSha256_ = {};
        lastReleasedBytes_ = bytes;
        lastReleaseReason_ = reason;
        return true;
    }

    void WorkerProcessTopologyBrokerRuntime::
        OnMemoryLimitChanging(
            uint64_t oldLimitBytes,
            uint64_t newLimitBytes)
    {
        std::lock_guard guard(mutex_);
        ++limitEventCount_;
        lastOldLimitBytes_ = oldLimitBytes;
        lastNewLimitBytes_ = newLimitBytes;
        auto usage = MemoryManager::AppMemoryUsage();
        auto reserve = SafetyReserve(lastNewLimitBytes_);
        if (
            allocation_ &&
            (lastNewLimitBytes_ <= reserve ||
                usage >
                    lastNewLimitBytes_ - reserve))
        {
            if (ReleaseAllocationLocked(
                L"memory_limit_changing"))
            {
                ++limitForcedReleaseCount_;
            }
        }
    }

    bool WorkerProcessTopologyBrokerRuntime::Release(
        std::wstring_view reason) noexcept
    {
        try
        {
            std::lock_guard guard(mutex_);
            return ReleaseAllocationLocked(reason);
        }
        catch (...)
        {
            return false;
        }
    }
}
