#pragma once

#include <filesystem>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerArtifactStore.h"

namespace XComputeProbe
{
    class WorkerPersistentComputeCoordinatorRuntime final
    {
    public:
        std::wstring Probe(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion);
        std::wstring ProbeContentAddressedStoreRecovery(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot);
        std::wstring RunProvableWorldsStreamingTiledV1(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot,
            WorkerArtifactStoreConfig const& artifactConfig,
            WorkerArtifactIdGenerator const& idGenerator);
        std::wstring RunStorageScaleCharacterizationV1(
            winrt::Windows::Data::Json::JsonObject const& request,
            std::wstring const& protocolVersion,
            std::filesystem::path const& workerRoot);
    };

    std::wstring WorkerPersistentComputeCoordinatorCapabilityJson();
}
