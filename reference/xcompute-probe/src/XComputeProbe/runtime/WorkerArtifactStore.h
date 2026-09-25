#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerArtifactStoreError : std::exception
    {
        std::string code;
        std::string message;

        WorkerArtifactStoreError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct PublishedArtifactResult
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

    struct ArtifactReadTarget
    {
        std::filesystem::path path;
        std::wstring artifactKind;
        std::wstring status;
        std::wstring sha256;
        uint64_t bytes = 0;
        bool committed = false;
    };

    struct WorkerContentAddressedBlobCommit
    {
        std::filesystem::path path;
        std::wstring sha256;
        uint64_t bytes = 0;
        bool deduplicated = false;
    };

    struct WorkerContentAddressedBlobCleanup
    {
        uint64_t candidateCount = 0;
        uint64_t removedBlobCount = 0;
        uint64_t removedBytes = 0;
        bool protectedByUnreadableMetadata = false;
    };

    struct WorkerArtifactStoreConfig
    {
        std::wstring protocolVersion;
        std::wstring artifactUploadSchemaVersion;
        std::wstring artifactBinaryFramingSchemaVersion;
        uint64_t maxArtifactJsonChunkBytes = 64 * 1024;
        uint64_t maxStreamWriteBytes = 4 * 1024 * 1024;
        uint64_t maxArtifactExpectedBytes = 256 * 1024 * 1024;
        uint64_t defaultArtifactRollbackHeadroomBytes = 64 * 1024 * 1024;
        uint64_t defaultArtifactWorkspaceBudgetBytes = 512 * 1024 * 1024;
        uint64_t maxArtifactWorkspaceBudgetBytes = 1024 * 1024 * 1024;
    };

    using WorkerArtifactIdGenerator = std::function<std::wstring()>;

    std::wstring NormalizeArtifactKind(std::wstring const& value);
    std::filesystem::path ArtifactRoot(std::filesystem::path const& root);
    std::filesystem::path ArtifactStagingDirectory(std::filesystem::path const& root, std::wstring const& artifactId);
    std::filesystem::path ArtifactManifestPath(std::filesystem::path const& root, std::wstring const& artifactId);
    std::filesystem::path ArtifactPayloadPath(std::filesystem::path const& root, std::wstring const& artifactId);
    std::filesystem::path ArtifactMetadataPath(std::filesystem::path const& root, std::wstring const& artifactId);
    std::filesystem::path ArtifactBlobPath(std::filesystem::path const& root, std::wstring const& sha256);
    std::wstring ArtifactHandleForBlob(std::filesystem::path const& root, std::filesystem::path const& blobPath);
    ArtifactReadTarget ResolveArtifactReadTarget(std::filesystem::path const& root, std::wstring const& artifactId);
    std::wstring WorkerContentSha256(std::vector<uint8_t> const& bytes);
    std::wstring WorkerContentSha256(std::string const& bytes);
    std::wstring WorkerContentSha256File(std::filesystem::path const& path);
    WorkerContentAddressedBlobCommit WorkerCommitContentAddressedBlob(
        std::filesystem::path const& root,
        std::wstring const& stagingToken,
        std::vector<uint8_t> const& bytes);
    uint64_t WorkerCountContentAddressedBlobReferences(
        std::filesystem::path const& root,
        std::wstring const& sha256);
    bool WorkerRemoveUnreferencedContentAddressedBlob(
        std::filesystem::path const& root,
        std::wstring const& sha256);
    WorkerContentAddressedBlobCleanup
    WorkerRemoveUnreferencedContentAddressedBlobs(
        std::filesystem::path const& root,
        std::vector<std::wstring> const& sha256);

    PublishedArtifactResult WorkerPublishUtf8ArtifactPayload(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator);

    std::wstring WorkerExecuteArtifactQuotaPreflight(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteBeginArtifactUpload(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator);
    std::wstring WorkerExecuteAppendArtifactChunk(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteAppendArtifactChunkBinary(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        std::vector<uint8_t> const& data,
        double authMs,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteCommitArtifactUpload(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteGenerateArtifactDataset(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config,
        WorkerArtifactIdGenerator const& idGenerator);
    std::wstring WorkerExecuteAbortArtifactUpload(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteDeleteArtifact(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteReapArtifacts(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
    std::wstring WorkerExecuteGetArtifactStatus(
        winrt::Windows::Data::Json::JsonObject const& request,
        std::filesystem::path const& root,
        WorkerArtifactStoreConfig const& config);
}
