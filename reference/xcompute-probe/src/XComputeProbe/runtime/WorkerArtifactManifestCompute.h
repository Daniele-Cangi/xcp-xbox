#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerArtifactManifestComputeError : std::exception
    {
        std::string code;
        std::string message;

        WorkerArtifactManifestComputeError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerArtifactManifestReadTarget
    {
        std::filesystem::path path;
        std::wstring artifactKind;
        std::wstring sha256;
        uint64_t bytes = 0;
        bool committed = false;
    };

    struct WorkerArtifactManifestPublishedResult
    {
        std::wstring artifactId;
        std::wstring artifactKind;
        std::wstring sha256;
        std::wstring blobHandle;
        std::wstring manifestPath;
        uint64_t bytes = 0;
        uint64_t workspaceBytesBefore = 0;
        uint64_t workspaceBytesAfter = 0;
        uint64_t workspaceBudgetBytes = 0;
        uint64_t rollbackHeadroomBytes = 0;
        uint64_t storageAvailableBytes = 0;
        bool storageAvailableKnown = false;
        bool deduplicated = false;
        double publishMs = 0.0;
    };

    struct WorkerArtifactManifestComputeInput
    {
        winrt::Windows::Data::Json::JsonObject request{ nullptr };
        std::wstring protocolVersion;
        std::wstring artifactUploadSchemaVersion;
        std::wstring artifactManifestComputeJobSchemaVersion;
        uint64_t maxArtifactManifestJsonBytes = 0;
        uint64_t maxArtifactManifestParts = 0;
        uint64_t maxArtifactExpectedBytes = 0;
        uint64_t readBufferBytes = 0;
    };

    using WorkerArtifactManifestResolver = std::function<WorkerArtifactManifestReadTarget(std::wstring const& artifactId)>;
    using WorkerArtifactManifestTextReader = std::function<std::string(std::filesystem::path const& path)>;
    using WorkerArtifactManifestPublisher = std::function<WorkerArtifactManifestPublishedResult(
        winrt::Windows::Data::Json::JsonObject const& publishRequest,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields)>;
    using WorkerArtifactManifestIdGenerator = std::function<std::wstring()>;

    std::wstring WorkerRunArtifactManifestComputeJob(
        WorkerArtifactManifestComputeInput const& input,
        WorkerArtifactManifestResolver const& resolver,
        WorkerArtifactManifestTextReader const& textReader,
        WorkerArtifactManifestPublisher const& publisher,
        WorkerArtifactManifestIdGenerator const& idGenerator);
}
