#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerGraphValidationError : std::exception
    {
        std::string code;
        std::string message;

        WorkerGraphValidationError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerGraphRequestValidation
    {
        winrt::Windows::Data::Json::JsonObject graph{ nullptr };
        winrt::Windows::Data::Json::JsonArray nodes{ nullptr };
        std::wstring graphId;
    };

    struct WorkerGraphNodeValidation
    {
        winrt::Windows::Data::Json::JsonObject node{ nullptr };
        std::wstring nodeId;
        std::wstring resultArtifactId;
    };

    struct WorkerGraphInputEdgeValidation
    {
        std::wstring fromNodeId;
        std::wstring role;
        std::wstring edgeType;
        std::wstring valueType;
        std::wstring expectedArtifactId;
        std::wstring edgeKey;
    };

    struct WorkerGraphInputEdgesValidation
    {
        winrt::Windows::Data::Json::JsonArray edges{ nullptr };
        bool present = false;
    };

    struct WorkerGraphBoundedLoopValidation
    {
        bool present = false;
        std::wstring profileId;
        std::wstring loopId;
        std::wstring initialNodeId;
        std::wstring finalNodeId;
        std::wstring carryEdgeRole;
        std::wstring finalEdgeRole;
        std::wstring profileJson = L"{}";
        std::vector<std::wstring> iterationNodeIds;
        uint64_t iterationCount = 0;
        uint64_t maxIterations = 0;
        uint64_t fuelPerIteration = 0;
        uint64_t fuelBudget = 0;
    };

    struct WorkerGraphStaticNeighborReadValidation
    {
        bool present = false;
        std::wstring profileId;
        std::wstring targetNodeId;
        std::wstring profileJson = L"{}";
        std::vector<std::wstring> requiredNeighborRoles;
        uint64_t neighborCount = 0;
        uint64_t maxNeighbors = 0;
    };

    std::wstring WorkerGraphOptionalString(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* name,
        std::wstring const& fallback = L"");

    bool WorkerGraphIsSafeId(std::wstring const& value);

    WorkerGraphRequestValidation WorkerGraphValidateRequest(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::wstring const& generatedGraphId,
        uint32_t maxNodeCount);

    WorkerGraphNodeValidation WorkerGraphValidateNodeValue(
        winrt::Windows::Data::Json::IJsonValue const& value,
        uint32_t nodeIndex,
        std::wstring const& graphId);

    void WorkerGraphValidateUniqueNodeId(bool duplicate);

    WorkerGraphInputEdgesValidation WorkerGraphValidateInputEdgesArray(
        winrt::Windows::Data::Json::JsonObject const& node);

    WorkerGraphInputEdgeValidation WorkerGraphValidateInputEdgeSource(
        winrt::Windows::Data::Json::IJsonValue const& value,
        uint64_t currentEdgeCount,
        uint32_t maxEdgeCount);

    void WorkerGraphValidateInputEdgeRole(
        WorkerGraphInputEdgeValidation const& edge,
        std::vector<std::wstring> const& existingEdgeKeys);

    std::wstring WorkerGraphValidateInputBindingMode(
        winrt::Windows::Data::Json::JsonObject const& node,
        bool hasResolvedInputEdges);

    WorkerGraphBoundedLoopValidation WorkerGraphValidateBoundedLoopProfile(
        winrt::Windows::Data::Json::JsonObject const& graph,
        winrt::Windows::Data::Json::JsonArray const& nodes);

    WorkerGraphStaticNeighborReadValidation WorkerGraphValidateStaticNeighborReadProfile(
        winrt::Windows::Data::Json::JsonObject const& graph,
        winrt::Windows::Data::Json::JsonArray const& nodes);
}
