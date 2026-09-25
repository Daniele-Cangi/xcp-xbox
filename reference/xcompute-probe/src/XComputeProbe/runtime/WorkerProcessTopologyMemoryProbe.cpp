#include "pch.h"
#include "WorkerProcessTopologyMemoryProbe.h"

#include "WorkerProcessTopologyProtocol.h"

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.System.Diagnostics.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::System;
using namespace Windows::System::Diagnostics;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {
        ProcessDiagnosticInfo CurrentProcessInfo()
        {
            try
            {
                auto process = ProcessDiagnosticInfo::GetForCurrentProcess();
                if (!process)
                {
                    Fail(
                        L"topology.memory_verification_failed",
                        L"ProcessDiagnosticInfo did not return the current process");
                }
                return process;
            }
            catch (Error const&)
            {
                throw;
            }
            catch (...)
            {
                Fail(
                    L"topology.memory_verification_failed",
                    L"ProcessDiagnosticInfo is unavailable for this packaged process");
            }
        }

        uint8_t Marker(uint64_t pageIndex, uint64_t generation)
        {
            return static_cast<uint8_t>(
                (pageIndex * 131ull + generation * 17ull + 0x5aull) & 0xffull);
        }

        std::wstring MarkerSha256(
            uint8_t const* memory,
            uint64_t pageBytes,
            uint64_t pageCount)
        {
            std::vector<uint8_t> markers;
            markers.reserve(static_cast<size_t>(pageCount));
            for (uint64_t page = 0; page < pageCount; ++page)
            {
                markers.push_back(memory[page * pageBytes]);
            }
            auto provider = HashAlgorithmProvider::OpenAlgorithm(
                HashAlgorithmNames::Sha256());
            auto value = std::wstring(CryptographicBuffer::EncodeToHexString(
                provider.HashData(
                    CryptographicBuffer::CreateFromByteArray(markers))).c_str());
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
            {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            return value;
        }

        struct HeartbeatSample
        {
            uint64_t index = 0;
            uint64_t startedUnixMilliseconds = 0;
            uint64_t completedUnixMilliseconds = 0;
            uint64_t gapMilliseconds = 0;
            std::wstring digestSha256;
            MemorySample memory;
        };
    }

    uint64_t UnixMilliseconds()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    ProcessIdentity CaptureProcessIdentity()
    {
        auto process = CurrentProcessInfo();
        ProcessIdentity identity;
        identity.processId = process.ProcessId();
        identity.executableFileName =
            std::wstring(process.ExecutableFileName().c_str());
        try
        {
            identity.applicationId = std::wstring(CoreApplication::Id().c_str());
        }
        catch (...)
        {
            identity.applicationId.clear();
        }
        if (identity.processId == 0 || identity.executableFileName.empty())
        {
            Fail(
                L"topology.memory_verification_failed",
                L"the foreground process identity is incomplete");
        }
        return identity;
    }

    MemorySample CaptureMemorySample()
    {
        try
        {
            auto process = CurrentProcessInfo();
            auto report = process.MemoryUsage().GetReport();
            MemorySample sample;
            sample.unixMilliseconds = UnixMilliseconds();
            sample.appUsageBytes = MemoryManager::AppMemoryUsage();
            sample.appLimitBytes = MemoryManager::AppMemoryUsageLimit();
            sample.appUsageLevel =
                static_cast<uint32_t>(MemoryManager::AppMemoryUsageLevel());
            sample.privatePageCountBytes = report.PrivatePageCount();
            sample.workingSetBytes = report.WorkingSetSizeInBytes();
            sample.peakWorkingSetBytes = report.PeakWorkingSetSizeInBytes();
            sample.pageFileBytes = report.PageFileSizeInBytes();
            sample.peakPageFileBytes = report.PeakPageFileSizeInBytes();
            sample.virtualMemoryBytes = report.VirtualMemorySizeInBytes();
            sample.peakVirtualMemoryBytes =
                report.PeakVirtualMemorySizeInBytes();
            sample.pageFaultCount = report.PageFaultCount();
            try
            {
                auto cpu = process.CpuUsage().GetReport();
                sample.cpuUserTime100ns =
                    static_cast<uint64_t>(cpu.UserTime().count());
                sample.cpuKernelTime100ns =
                    static_cast<uint64_t>(cpu.KernelTime().count());
                sample.cpuReportAvailable = true;
            }
            catch (...)
            {
                sample.cpuReportAvailable = false;
            }
            return sample;
        }
        catch (Error const&)
        {
            throw;
        }
        catch (...)
        {
            Fail(
                L"topology.memory_verification_failed",
                L"the packaged process memory report could not be captured");
        }
    }

    JsonObject MemorySampleObject(MemorySample const& sample)
    {
        JsonObject value;
        PutNumber(
            value,
            L"sample_unix_milliseconds",
            sample.unixMilliseconds);
        PutNumber(
            value,
            L"app_memory_usage_bytes",
            sample.appUsageBytes);
        PutNumber(
            value,
            L"app_memory_usage_limit_bytes",
            sample.appLimitBytes);
        PutNumber(
            value,
            L"app_memory_usage_level",
            sample.appUsageLevel);
        PutNumber(
            value,
            L"process_private_page_count_bytes",
            sample.privatePageCountBytes);
        PutNumber(
            value,
            L"process_working_set_bytes",
            sample.workingSetBytes);
        PutNumber(
            value,
            L"process_peak_working_set_bytes",
            sample.peakWorkingSetBytes);
        PutNumber(
            value,
            L"process_virtual_memory_bytes",
            sample.virtualMemoryBytes);
        PutNumber(
            value,
            L"process_page_fault_count",
            sample.pageFaultCount);
        PutBool(
            value,
            L"process_cpu_report_available",
            sample.cpuReportAvailable);
        PutNumber(
            value,
            L"process_cpu_user_time_100ns",
            sample.cpuUserTime100ns);
        PutNumber(
            value,
            L"process_cpu_kernel_time_100ns",
            sample.cpuKernelTime100ns);
        return value;
    }

    struct MeasuredHold::Impl
    {
        Impl(
            uint64_t requestedBytes,
            uint64_t expectedHeartbeatCount,
            uint64_t holdIntervalMilliseconds)
            : requestedBytes(requestedBytes),
              expectedHeartbeatCount(expectedHeartbeatCount),
              holdIntervalMilliseconds(holdIntervalMilliseconds)
        {
            SYSTEM_INFO systemInfo{};
            GetSystemInfo(&systemInfo);
            pageBytes = systemInfo.dwPageSize;
            if (pageBytes == 0)
            {
                Fail(
                    L"topology.memory_allocation_failed",
                    L"the system page size was unavailable");
            }
            committedBytes =
                ((requestedBytes + pageBytes - 1) / pageBytes) * pageBytes;
            pageCount = committedBytes / pageBytes;
            memory = static_cast<uint8_t*>(VirtualAllocFromApp(
                nullptr,
                static_cast<SIZE_T>(committedBytes),
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE));
            if (!memory)
            {
                Fail(
                    L"topology.memory_allocation_failed",
                    L"the bounded foreground memory allocation failed");
            }

            MEMORY_BASIC_INFORMATION information{};
            auto queryBytes =
                VirtualQuery(memory, &information, sizeof(information));
            querySucceeded = queryBytes == sizeof(information);
            queryRegionBytes = querySucceeded
                ? static_cast<uint64_t>(information.RegionSize)
                : 0;
            queryState = querySucceeded ? information.State : 0;
            queryProtect = querySucceeded ? information.Protect : 0;
            queryType = querySucceeded ? information.Type : 0;
            if (!querySucceeded || queryState != MEM_COMMIT ||
                queryRegionBytes < committedBytes)
            {
                VirtualFree(memory, 0, MEM_RELEASE);
                memory = nullptr;
                Fail(
                    L"topology.memory_allocation_failed",
                    L"VirtualQuery did not observe the complete committed allocation");
            }
        }

        ~Impl()
        {
            JoinNoThrow();
            if (memory)
            {
                VirtualFree(memory, 0, MEM_RELEASE);
            }
        }

        void Start()
        {
            if (heartbeatThread.joinable() || startedUnixMilliseconds != 0)
            {
                Fail(
                    L"topology.memory_verification_failed",
                    L"the foreground heartbeat sequence was started more than once");
            }
            heartbeatThread = std::thread([this]()
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    Run();
                    winrt::uninit_apartment();
                }
                catch (...)
                {
                    endedUnixMilliseconds = UnixMilliseconds();
                    failure = std::current_exception();
                }
            });
        }

        void Join()
        {
            if (heartbeatThread.joinable())
            {
                heartbeatThread.join();
            }
            if (failure)
            {
                std::rethrow_exception(failure);
            }
        }

        void JoinNoThrow() noexcept
        {
            try
            {
                Join();
            }
            catch (...)
            {
            }
        }

        bool Release()
        {
            Join();
            if (!memory)
            {
                return true;
            }
            if (!VirtualFree(memory, 0, MEM_RELEASE))
            {
                return false;
            }
            memory = nullptr;
            return true;
        }

        void Run() noexcept
        {
            try
            {
                startedUnixMilliseconds = UnixMilliseconds();
                uint64_t previousStarted = 0;
                for (uint64_t index = 1;
                     index <= expectedHeartbeatCount;
                     ++index)
                {
                    HeartbeatSample sample;
                    sample.index = index;
                    sample.startedUnixMilliseconds = UnixMilliseconds();
                    sample.gapMilliseconds = previousStarted == 0
                        ? 0
                        : sample.startedUnixMilliseconds - previousStarted;
                    maximumGapMilliseconds = (std::max)(
                        maximumGapMilliseconds,
                        sample.gapMilliseconds);
                    previousStarted = sample.startedUnixMilliseconds;

                    if (generation != 0)
                    {
                        VerifyGeneration(generation);
                        ++retentionVerificationCount;
                    }
                    generation = index;
                    TouchAndVerify(generation);
                    if (mismatchCount != 0)
                    {
                        Fail(
                            L"topology.memory_verification_failed",
                            L"a foreground page marker failed verification");
                    }
                    sample.digestSha256 =
                        MarkerSha256(memory, pageBytes, pageCount);
                    if (!IsLowerHex(sample.digestSha256, 64))
                    {
                        Fail(
                            L"topology.memory_verification_failed",
                            L"the foreground SHA-256 digest is invalid");
                    }
                    sample.memory = CaptureMemorySample();
                    sample.completedUnixMilliseconds = UnixMilliseconds();
                    samples.push_back(std::move(sample));
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            holdIntervalMilliseconds));
                }
                VerifyGeneration(generation);
                ++retentionVerificationCount;
                endedUnixMilliseconds = UnixMilliseconds();
            }
            catch (...)
            {
                endedUnixMilliseconds = UnixMilliseconds();
                failure = std::current_exception();
            }
        }

        void VerifyGeneration(uint64_t currentGeneration)
        {
            for (uint64_t page = 0; page < pageCount; ++page)
            {
                if (memory[page * pageBytes] != Marker(page, currentGeneration))
                {
                    ++mismatchCount;
                }
            }
            if (mismatchCount != 0)
            {
                Fail(
                    L"topology.memory_verification_failed",
                    L"a foreground page marker changed between heartbeat windows");
            }
        }

        void TouchAndVerify(uint64_t currentGeneration)
        {
            for (uint64_t page = 0; page < pageCount; ++page)
            {
                memory[page * pageBytes] =
                    Marker(page, currentGeneration);
            }
            for (uint64_t page = 0; page < pageCount; ++page)
            {
                if (memory[page * pageBytes] !=
                    Marker(page, currentGeneration))
                {
                    ++mismatchCount;
                }
            }
        }

        JsonObject HeartbeatsJson() const
        {
            JsonObject value;
            PutNumber(
                value,
                L"interval_ms",
                holdIntervalMilliseconds);
            PutNumber(
                value,
                L"samples_expected",
                expectedHeartbeatCount);
            PutNumber(
                value,
                L"samples_observed",
                samples.size());
            PutNumber(
                value,
                L"maximum_gap_ms",
                maximumGapMilliseconds);
            PutNumber(
                value,
                L"retention_verification_count",
                retentionVerificationCount);
            PutNumber(
                value,
                L"started_unix_milliseconds",
                startedUnixMilliseconds);
            PutNumber(
                value,
                L"ended_unix_milliseconds",
                endedUnixMilliseconds);
            PutBool(
                value,
                L"independent_while_memory_retained",
                true);

            JsonArray sampleValues;
            for (auto const& sample : samples)
            {
                JsonObject item;
                PutNumber(item, L"index", sample.index);
                PutNumber(
                    item,
                    L"started_unix_milliseconds",
                    sample.startedUnixMilliseconds);
                PutNumber(
                    item,
                    L"completed_unix_milliseconds",
                    sample.completedUnixMilliseconds);
                PutNumber(item, L"gap_ms", sample.gapMilliseconds);
                PutString(
                    item,
                    L"digest_sha256",
                    sample.digestSha256);
                item.SetNamedValue(
                    L"memory",
                    MemorySampleObject(sample.memory));
                sampleValues.Append(item);
            }
            value.SetNamedValue(L"samples", sampleValues);
            return value;
        }

        uint8_t* memory = nullptr;
        uint64_t requestedBytes = 0;
        uint64_t committedBytes = 0;
        uint64_t pageBytes = 0;
        uint64_t pageCount = 0;
        uint64_t expectedHeartbeatCount = 0;
        uint64_t holdIntervalMilliseconds = 0;
        uint64_t generation = 0;
        uint64_t maximumGapMilliseconds = 0;
        uint64_t mismatchCount = 0;
        uint64_t retentionVerificationCount = 0;
        uint64_t startedUnixMilliseconds = 0;
        uint64_t endedUnixMilliseconds = 0;
        bool querySucceeded = false;
        uint64_t queryRegionBytes = 0;
        uint32_t queryState = 0;
        uint32_t queryProtect = 0;
        uint32_t queryType = 0;
        std::vector<HeartbeatSample> samples;
        std::exception_ptr failure;
        std::thread heartbeatThread;
    };

    MeasuredHold::MeasuredHold(
        uint64_t requestedBytes,
        uint64_t expectedHeartbeatCount,
        uint64_t holdIntervalMilliseconds)
        : impl_(std::make_unique<Impl>(
              requestedBytes,
              expectedHeartbeatCount,
              holdIntervalMilliseconds))
    {
    }

    MeasuredHold::~MeasuredHold() = default;

    void MeasuredHold::Start() { impl_->Start(); }
    void MeasuredHold::Join() { impl_->Join(); }
    bool MeasuredHold::Release() { return impl_->Release(); }

    JsonObject MeasuredHold::HeartbeatsJson() const
    {
        return impl_->HeartbeatsJson();
    }

    std::wstring MeasuredHold::DigestSha256() const
    {
        return impl_->samples.empty()
            ? L""
            : impl_->samples.back().digestSha256;
    }

    uint64_t MeasuredHold::RequestedBytes() const
    {
        return impl_->requestedBytes;
    }
    uint64_t MeasuredHold::CommittedBytes() const
    {
        return impl_->committedBytes;
    }
    uint64_t MeasuredHold::PageBytes() const { return impl_->pageBytes; }
    uint64_t MeasuredHold::PageCount() const { return impl_->pageCount; }
    uint64_t MeasuredHold::Generation() const { return impl_->generation; }
    uint64_t MeasuredHold::HeartbeatCount() const
    {
        return impl_->samples.size();
    }
    uint64_t MeasuredHold::ExpectedHeartbeatCount() const
    {
        return impl_->expectedHeartbeatCount;
    }
    uint64_t MeasuredHold::MaximumGapMilliseconds() const
    {
        return impl_->maximumGapMilliseconds;
    }
    uint64_t MeasuredHold::MismatchCount() const
    {
        return impl_->mismatchCount;
    }
    uint64_t MeasuredHold::StartedUnixMilliseconds() const
    {
        return impl_->startedUnixMilliseconds;
    }
    uint64_t MeasuredHold::EndedUnixMilliseconds() const
    {
        return impl_->endedUnixMilliseconds;
    }
    bool MeasuredHold::QuerySucceeded() const
    {
        return impl_->querySucceeded;
    }
    uint64_t MeasuredHold::QueryRegionBytes() const
    {
        return impl_->queryRegionBytes;
    }
    uint32_t MeasuredHold::QueryState() const
    {
        return impl_->queryState;
    }
    uint32_t MeasuredHold::QueryProtect() const
    {
        return impl_->queryProtect;
    }
    uint32_t MeasuredHold::QueryType() const
    {
        return impl_->queryType;
    }
}