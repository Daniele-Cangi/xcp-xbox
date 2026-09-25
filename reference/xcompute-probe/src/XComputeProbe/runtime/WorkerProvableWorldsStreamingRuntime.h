#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerArtifactStore.h"
#include "WorkerContentAddressedStoreRecovery.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const*
        WorkerProvableWorldsStreamingRequestSchemaVersion =
            L"xcp-provable-worlds-streaming-tiled-v1-runtime-request";
    inline constexpr wchar_t const*
        WorkerProvableWorldsStreamingResultSchemaVersion =
            L"xcp-provable-worlds-streaming-tiled-v1-runtime-result";
    inline constexpr wchar_t const*
        WorkerProvableWorldsStreamingTrainId =
            L"PROVABLE_WORLDS_STREAMING_PRODUCT_ADMISSION_V1";

    struct WorkerProvableWorldsStreamingError : std::exception
    {
        std::wstring code;
        std::string message;

        WorkerProvableWorldsStreamingError(
            std::wstring codeValue,
            std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerProvableWorldsRestartAuthorization final
    {
        std::wstring persistedReservationId;
        uint64_t persistedGeneration = 0;
        std::wstring activeReservationId;
        uint64_t activeGeneration = 0;
        bool workerBrokerBindingAgreed = false;
    };

    class WorkerProvableWorldsStreamingRuntime final
    {
    public:
        winrt::Windows::Data::Json::JsonObject Execute(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            WorkerProvableWorldsRestartAuthorization const*
                restartAuthorization,
            std::wstring const& operation,
            std::wstring const& fieldArtifactId,
            WorkerArtifactStoreConfig const& artifactConfig,
            WorkerArtifactIdGenerator const& idGenerator) const;
    };

    std::wstring WorkerProvableWorldsStreamingCapabilityJson();
}
