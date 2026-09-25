#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerContentAddressedStoreRecovery.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const*
        WorkerStorageScaleCharacterizationRequestSchemaVersion =
            L"xcp-storage-scale-characterization-v1-runtime-request";
    inline constexpr wchar_t const*
        WorkerStorageScaleCharacterizationResultSchemaVersion =
            L"xcp-storage-scale-characterization-v1-runtime-result";
    inline constexpr wchar_t const*
        WorkerStorageScaleCharacterizationTrainId =
            L"STORAGE_SCALE_CHARACTERIZATION_V1";

    struct WorkerStorageScaleCharacterizationError :
        std::exception
    {
        std::wstring code;
        std::string message;

        WorkerStorageScaleCharacterizationError(
            std::wstring codeValue,
            std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerStorageScaleRestartAuthorization final
    {
        std::wstring persistedReservationId;
        uint64_t persistedGeneration = 0;
        std::wstring activeReservationId;
        uint64_t activeGeneration = 0;
        bool workerBrokerBindingAgreed = false;
    };

    class WorkerStorageScaleCharacterizationRuntime final
    {
    public:
        winrt::Windows::Data::Json::JsonObject Execute(
            std::filesystem::path const& workerRoot,
            WorkerPersistentStorageBinding const& binding,
            WorkerStorageScaleRestartAuthorization const*
                restartAuthorization,
            std::wstring const& operation) const;
    };

    std::wstring
    WorkerStorageScaleCharacterizationCapabilityJson();
}
