#pragma once

#include <cstdint>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerGraphExpansionApplication
    {
        winrt::Windows::Data::Json::JsonObject graph{ nullptr };
        winrt::Windows::Data::Json::JsonArray nodes{ nullptr };
        std::wstring profileId;
        std::wstring targetNodeId;
        std::wstring manifestArtifactId;
        std::wstring profileJson = L"{}";
        uint64_t preNodeCount = 0;
        uint64_t expandedNodeCount = 0;
        uint64_t expandedEdgeCount = 0;
        bool present = false;
    };

    WorkerGraphExpansionApplication WorkerGraphApplyBoundedStaticExpansion(
        winrt::Windows::Data::Json::JsonObject const& graph,
        winrt::Windows::Data::Json::JsonArray const& nodes);
}
