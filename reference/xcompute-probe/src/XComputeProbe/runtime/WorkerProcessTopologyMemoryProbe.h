#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe::ProcessTopology
{
    struct ProcessIdentity
    {
        uint32_t processId = 0;
        std::wstring executableFileName;
        std::wstring applicationId;
    };

    struct MemorySample
    {
        uint64_t unixMilliseconds = 0;
        uint64_t appUsageBytes = 0;
        uint64_t appLimitBytes = 0;
        uint32_t appUsageLevel = 0;
        uint64_t privatePageCountBytes = 0;
        uint64_t workingSetBytes = 0;
        uint64_t peakWorkingSetBytes = 0;
        uint64_t pageFileBytes = 0;
        uint64_t peakPageFileBytes = 0;
        uint64_t virtualMemoryBytes = 0;
        uint64_t peakVirtualMemoryBytes = 0;
        uint64_t pageFaultCount = 0;
        bool cpuReportAvailable = false;
        uint64_t cpuUserTime100ns = 0;
        uint64_t cpuKernelTime100ns = 0;
    };

    struct LimitChange
    {
        uint64_t unixMilliseconds = 0;
        uint64_t oldLimitBytes = 0;
        uint64_t newLimitBytes = 0;
    };

    uint64_t UnixMilliseconds();
    ProcessIdentity CaptureProcessIdentity();
    MemorySample CaptureMemorySample();
    winrt::Windows::Data::Json::JsonObject MemorySampleObject(
        MemorySample const& sample);

    class MeasuredHold
    {
    public:
        MeasuredHold(
            uint64_t requestedBytes,
            uint64_t expectedHeartbeatCount,
            uint64_t holdIntervalMilliseconds);
        ~MeasuredHold();

        MeasuredHold(MeasuredHold const&) = delete;
        MeasuredHold& operator=(MeasuredHold const&) = delete;
        MeasuredHold(MeasuredHold&&) = delete;
        MeasuredHold& operator=(MeasuredHold&&) = delete;

        void Start();
        void Join();
        bool Release();

        winrt::Windows::Data::Json::JsonObject HeartbeatsJson() const;
        std::wstring DigestSha256() const;

        uint64_t RequestedBytes() const;
        uint64_t CommittedBytes() const;
        uint64_t PageBytes() const;
        uint64_t PageCount() const;
        uint64_t Generation() const;
        uint64_t HeartbeatCount() const;
        uint64_t ExpectedHeartbeatCount() const;
        uint64_t MaximumGapMilliseconds() const;
        uint64_t MismatchCount() const;
        uint64_t StartedUnixMilliseconds() const;
        uint64_t EndedUnixMilliseconds() const;
        bool QuerySucceeded() const;
        uint64_t QueryRegionBytes() const;
        uint32_t QueryState() const;
        uint32_t QueryProtect() const;
        uint32_t QueryType() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}