#include "pch.h"
#include "WorkerGraphResourceLedger.h"

using namespace winrt::Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        constexpr uint64_t MaxGraphFuelValue = 16ull * 1024ull * 1024ull;
        constexpr uint64_t MaxGraphXvmMemoryBytesValue = 64ull * 1024ull * 1024ull;
        constexpr uint64_t MaxGraphXvmOutputBytesValue = 4ull * 1024ull * 1024ull;
        constexpr uint64_t DefaultGraphFuelValue = 1000ull * 1000ull;
        constexpr uint64_t DefaultGraphXvmMemoryBytesValue = 1024ull * 1024ull;
        constexpr uint64_t DefaultGraphXvmOutputBytesValue = 64ull * 1024ull;

        uint64_t ReadBoundedInteger(
            JsonObject const& object,
            wchar_t const* name,
            uint64_t fallback,
            uint64_t minimum,
            uint64_t maximum,
            char const* code)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto raw = object.GetNamedNumber(name);
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < static_cast<double>(minimum) || raw > static_cast<double>(maximum))
            {
                throw WorkerGraphResourceError(code, "resource value is outside the admitted integer range");
            }
            return static_cast<uint64_t>(raw);
        }

        JsonObject ReadObject(JsonObject const& parent, wchar_t const* name, bool required, char const* code)
        {
            if (!parent.HasKey(name))
            {
                if (required)
                {
                    throw WorkerGraphResourceError(code, "required resource object is missing");
                }
                return JsonObject();
            }
            auto value = parent.GetNamedValue(name);
            if (value.ValueType() != JsonValueType::Object)
            {
                throw WorkerGraphResourceError(code, "resource field must be a JSON object");
            }
            return value.GetObject();
        }

        void CheckedReserve(uint64_t& value, uint64_t addition, uint64_t limit, char const* code)
        {
            if (addition > limit || value > limit - addition)
            {
                throw WorkerGraphResourceError(code, "graph resource reservations exceed admitted policy");
            }
            value += addition;
        }
    }

    WorkerGraphResourceError::WorkerGraphResourceError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue))
    {
    }

    char const* WorkerGraphResourceError::what() const noexcept
    {
        return message.c_str();
    }

    uint64_t WorkerGraphMaxFuel()
    {
        return MaxGraphFuelValue;
    }

    uint64_t WorkerGraphMaxXvmMemoryBytes()
    {
        return MaxGraphXvmMemoryBytesValue;
    }

    uint64_t WorkerGraphMaxXvmOutputBytes()
    {
        return MaxGraphXvmOutputBytesValue;
    }

    WorkerGraphNodeResourceLimits WorkerGraphReadNodeResourceLimits(JsonObject const& node, bool required)
    {
        auto resources = ReadObject(node, L"resource_limits", required, "submit_graph.resource_limits_invalid");
        if (!resources)
        {
            return {};
        }

        WorkerGraphNodeResourceLimits limits;
        limits.fuel = ReadBoundedInteger(resources, L"fuel", 0, 1, MaxGraphFuelValue, "submit_graph.fuel_invalid");
        limits.memoryBytes = ReadBoundedInteger(resources, L"memory_bytes", 0, 4, MaxGraphXvmMemoryBytesValue, "submit_graph.memory_invalid");
        limits.outputBytes = ReadBoundedInteger(resources, L"output_bytes", 0, 4, MaxGraphXvmOutputBytesValue, "submit_graph.output_invalid");
        if (required && (limits.fuel == 0 || limits.memoryBytes == 0 || limits.outputBytes == 0))
        {
            throw WorkerGraphResourceError("submit_graph.resource_limits_required", "metered graph nodes require fuel, memory_bytes and output_bytes");
        }
        return limits;
    }

    WorkerGraphResourceLedger WorkerGraphAdmitResourceLedger(JsonObject const& graph, JsonArray const& nodes)
    {
        WorkerGraphResourceLedger ledger;
        auto policy = ReadObject(graph, L"resource_policy", false, "submit_graph.resource_policy_invalid");
        ledger.fuelLimit = policy
            ? ReadBoundedInteger(policy, L"fuel", DefaultGraphFuelValue, 1, MaxGraphFuelValue, "submit_graph.graph_fuel_invalid")
            : DefaultGraphFuelValue;
        ledger.memoryLimitBytes = policy
            ? ReadBoundedInteger(policy, L"memory_bytes", DefaultGraphXvmMemoryBytesValue, 4, MaxGraphXvmMemoryBytesValue, "submit_graph.graph_memory_invalid")
            : DefaultGraphXvmMemoryBytesValue;
        ledger.outputLimitBytes = policy
            ? ReadBoundedInteger(policy, L"output_bytes", DefaultGraphXvmOutputBytesValue, 4, MaxGraphXvmOutputBytesValue, "submit_graph.graph_output_invalid")
            : DefaultGraphXvmOutputBytesValue;

        for (uint32_t index = 0; index < nodes.Size(); ++index)
        {
            auto value = nodes.GetAt(index);
            if (value.ValueType() != JsonValueType::Object)
            {
                continue;
            }
            auto node = value.GetObject();
            auto command = std::wstring(node.GetNamedString(L"command", L"").c_str());
            auto meteredNode = command == L"xvm_program" || command == L"gpu_spmd_precompiled";
            if (!meteredNode)
            {
                continue;
            }

            auto limits = WorkerGraphReadNodeResourceLimits(node, true);
            CheckedReserve(ledger.reservedFuel, limits.fuel, ledger.fuelLimit, "submit_graph.graph_fuel_exceeded");
            CheckedReserve(ledger.reservedMemoryBytes, limits.memoryBytes, ledger.memoryLimitBytes, "submit_graph.graph_memory_exceeded");
            CheckedReserve(ledger.reservedOutputBytes, limits.outputBytes, ledger.outputLimitBytes, "submit_graph.graph_output_exceeded");
            if (command == L"xvm_program")
            {
                ++ledger.xvmNodeCount;
            }
            else
            {
                ++ledger.gpuSpmdNodeCount;
            }
        }
        return ledger;
    }

    void WorkerGraphRecordResourceUsage(
        WorkerGraphResourceLedger& ledger,
        uint64_t fuelConsumed,
        uint64_t memoryBytes,
        uint64_t outputBytes)
    {
        if (fuelConsumed > ledger.fuelLimit - ledger.consumedFuel)
        {
            throw WorkerGraphResourceError("submit_graph.graph_fuel_exhausted", "runtime fuel consumption exceeded graph policy");
        }
        if (memoryBytes > ledger.memoryLimitBytes || outputBytes > ledger.outputLimitBytes - ledger.producedOutputBytes)
        {
            throw WorkerGraphResourceError("submit_graph.graph_resource_exhausted", "runtime resource consumption exceeded graph policy");
        }
        ledger.consumedFuel += fuelConsumed;
        ledger.peakMemoryBytes = (std::max)(ledger.peakMemoryBytes, memoryBytes);
        ledger.producedOutputBytes += outputBytes;
    }

    std::wstring WorkerGraphResourceUsageJson(uint64_t fuelConsumed, uint64_t memoryBytes, uint64_t outputBytes)
    {
        return L"{\"fuel_consumed\":" + std::to_wstring(fuelConsumed) +
            L",\"memory_bytes\":" + std::to_wstring(memoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(outputBytes) + L"}";
    }

    std::wstring WorkerGraphResourceLedgerJson(WorkerGraphResourceLedger const& ledger)
    {
        return std::wstring(L"{\"schema_version\":\"worker-graph-resource-ledger-0.1\"") +
            L",\"policy\":{\"fuel\":" + std::to_wstring(ledger.fuelLimit) +
            L",\"memory_bytes\":" + std::to_wstring(ledger.memoryLimitBytes) +
            L",\"output_bytes\":" + std::to_wstring(ledger.outputLimitBytes) + L"}" +
            L",\"reserved\":{\"fuel\":" + std::to_wstring(ledger.reservedFuel) +
            L",\"memory_bytes\":" + std::to_wstring(ledger.reservedMemoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(ledger.reservedOutputBytes) + L"}" +
            L",\"consumed\":{\"fuel\":" + std::to_wstring(ledger.consumedFuel) +
            L",\"peak_memory_bytes\":" + std::to_wstring(ledger.peakMemoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(ledger.producedOutputBytes) + L"}" +
            L",\"xvm_node_count\":" + std::to_wstring(ledger.xvmNodeCount) +
            L",\"gpu_spmd_node_count\":" + std::to_wstring(ledger.gpuSpmdNodeCount) + L"}";
    }
}
