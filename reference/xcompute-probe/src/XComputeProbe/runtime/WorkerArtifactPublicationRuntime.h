#pragma once

#include <exception>
#include <filesystem>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerArtifactStore.h"
#include "WorkerAsyncJobRuntime.h"
#include "WorkerGraphPublishing.h"

namespace XComputeProbe
{
    struct WorkerArtifactPublicationError : std::exception
    {
        std::string code;
        std::string message;

        WorkerArtifactPublicationError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerArtifactPublicationConfig
    {
        std::wstring protocolVersion;
        std::wstring artifactUploadSchemaVersion;
        std::wstring jobResultArtifactSchemaVersion;
        WorkerArtifactStoreConfig artifactStore;
        WorkerArtifactIdGenerator artifactIdGenerator;
    };

    using WorkerArtifactJobReader = std::function<WorkerAsyncJobRecord(
        std::filesystem::path const& root,
        std::wstring const& jobId)>;

    WorkerGraphPublishedArtifact WorkerPublishGraphArtifact(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields,
        WorkerArtifactPublicationConfig const& config);

    std::wstring WorkerPublishJobResultArtifact(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactPublicationConfig const& config,
        WorkerArtifactJobReader const& jobReader);
}
