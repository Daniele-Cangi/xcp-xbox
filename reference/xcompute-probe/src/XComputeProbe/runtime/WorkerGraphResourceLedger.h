#pragma once

#include <cstdint>
#include <exception>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerGraphResourceError : std::exception
    {
        std::string code;
        std::string message;

        WorkerGraphResourceError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerGraphNodeResourceLimits
    {
        uint64_t fuel = 0;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
    };

    struct WorkerGraphResourceLedger
    {
        uint64_t fuelLimit = 0;
        uint64_t memoryLimitBytes = 0;
        uint64_t outputLimitBytes = 0;
        uint64_t reservedFuel = 0;
        uint64_t reservedMemoryBytes = 0;
        uint64_t reservedOutputBytes = 0;
        uint64_t consumedFuel = 0;
        uint64_t peakMemoryBytes = 0;
        uint64_t producedOutputBytes = 0;
        uint64_t xvmNodeCount = 0;
        uint64_t gpuSpmdNodeCount = 0;
    };

    uint64_t WorkerGraphMaxFuel();
    uint64_t WorkerGraphMaxXvmMemoryBytes();
    uint64_t WorkerGraphMaxXvmOutputBytes();

    WorkerGraphNodeResourceLimits WorkerGraphReadNodeResourceLimits(
        winrt::Windows::Data::Json::JsonObject const& node,
        bool required);

    WorkerGraphResourceLedger WorkerGraphAdmitResourceLedger(
        winrt::Windows::Data::Json::JsonObject const& graph,
        winrt::Windows::Data::Json::JsonArray const& nodes);

    void WorkerGraphRecordResourceUsage(
        WorkerGraphResourceLedger& ledger,
        uint64_t fuelConsumed,
        uint64_t memoryBytes,
        uint64_t outputBytes);

    std::wstring WorkerGraphResourceUsageJson(
        uint64_t fuelConsumed,
        uint64_t memoryBytes,
        uint64_t outputBytes);

    std::wstring WorkerGraphResourceLedgerJson(WorkerGraphResourceLedger const& ledger);
}
