#include "pch.h"
#include "WorkerArtifactPublicationRuntime.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    WorkerArtifactPublicationError::WorkerArtifactPublicationError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)), message(std::move(messageValue))
    {
    }

    char const* WorkerArtifactPublicationError::what() const noexcept
    {
        return message.c_str();
    }

    namespace
    {
        std::string WideToUtf8Local(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::wstring GetOptionalStringLocal(JsonObject const& object, wchar_t const* name, std::wstring const& fallback = L"")
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto value = object.GetNamedString(name, fallback);
            return std::wstring(value.data(), value.size());
        }

        std::wstring BoolJsonLocal(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring DoubleJsonLocal(double value, int precision)
        {
            std::wostringstream out;
            out.imbue(std::locale::classic());
            out << std::fixed << std::setprecision(precision) << value;
            return out.str();
        }

        std::wstring OkBaseLocal(std::wstring const& command, WorkerArtifactPublicationConfig const& config)
        {
            return L"{\"ok\":true,\"protocol_version\":" + JsonString(config.protocolVersion) +
                L",\"command\":" + JsonString(command);
        }

        WorkerGraphPublishedArtifact MoveGraphArtifact(PublishedArtifactResult const& published)
        {
            WorkerGraphPublishedArtifact artifact;
            artifact.artifactId = published.artifactId;
            artifact.artifactKind = published.artifactKind;
            artifact.sha256 = published.sha256;
            artifact.blobHandle = published.blobHandle;
            artifact.manifestPath = published.manifestPath;
            artifact.bytes = published.bytes;
            artifact.workspaceBytesBefore = published.workspaceBytesBefore;
            artifact.workspaceBytesAfter = published.workspaceBytesAfter;
            artifact.workspaceBudgetBytes = published.workspaceBudgetBytes;
            artifact.rollbackHeadroomBytes = published.rollbackHeadroomBytes;
            artifact.storageAvailableBytes = published.storageAvailableBytes;
            artifact.storageAvailableKnown = published.storageAvailableKnown;
            artifact.deduplicated = published.deduplicated;
            artifact.publishMs = published.publishMs;
            return artifact;
        }
    }

    WorkerGraphPublishedArtifact WorkerPublishGraphArtifact(
        JsonObject const& request,
        fs::path const& root,
        std::wstring const& defaultArtifactId,
        std::wstring const& defaultArtifactKind,
        std::string const& payloadUtf8,
        std::wstring const& extraManifestFields,
        WorkerArtifactPublicationConfig const& config)
    {
        return MoveGraphArtifact(WorkerPublishUtf8ArtifactPayload(
            request,
            root,
            defaultArtifactId,
            defaultArtifactKind,
            payloadUtf8,
            extraManifestFields,
            config.artifactStore,
            config.artifactIdGenerator));
    }

    std::wstring WorkerPublishJobResultArtifact(
        JsonObject const& request,
        fs::path const& root,
        WorkerArtifactPublicationConfig const& config,
        WorkerArtifactJobReader const& jobReader)
    {
        auto jobId = GetOptionalStringLocal(request, L"job_id");
        if (jobId.empty())
        {
            throw WorkerArtifactPublicationError("job_id.required", "job_id is required");
        }
        if (!jobReader)
        {
            throw WorkerArtifactPublicationError("job.reader_unavailable", "async job reader is unavailable");
        }

        auto record = jobReader(root, jobId);
        if (!record.ok)
        {
            throw WorkerArtifactPublicationError(
                WideToUtf8Local(record.operationErrorCode),
                WideToUtf8Local(record.operationErrorMessage));
        }
        if (!record.completed)
        {
            throw WorkerArtifactPublicationError("job.not_complete", "job result artifact requires a terminal async job");
        }
        if (record.resultJson.empty())
        {
            throw WorkerArtifactPublicationError("job.result_unavailable", "job result artifact requires a persisted result object");
        }

        try
        {
            JsonObject::Parse(record.resultJson);
            JsonObject::Parse(record.jobStatusJson);
        }
        catch (...)
        {
            throw WorkerArtifactPublicationError("job.result_invalid", "stored async job result JSON could not be parsed");
        }

        auto payload =
            L"{\"schema_version\":" + JsonString(config.jobResultArtifactSchemaVersion) +
            L",\"protocol_version\":" + JsonString(config.protocolVersion) +
            L",\"job_id\":" + JsonString(jobId) +
            L",\"source_async_schema\":" + JsonString(record.sourceSchema) +
            L",\"source_persisted\":" + BoolJsonLocal(record.persisted) +
            L",\"job_status\":" + record.jobStatusJson +
            L",\"result\":" + record.resultJson +
            L"}";
        auto payloadUtf8 = WideToUtf8Local(payload);
        auto sourceResultBytes = static_cast<uint64_t>(WideToUtf8Local(record.resultJson).size());
        auto manifestFields =
            L",\"job_output_schema\":" + JsonString(config.jobResultArtifactSchemaVersion) +
            L",\"job_id\":" + JsonString(jobId) +
            L",\"job_status\":" + JsonString(record.status) +
            L",\"source_async_schema\":" + JsonString(record.sourceSchema) +
            L",\"source_snapshot_persisted\":" + BoolJsonLocal(record.snapshotPersisted);

        auto published = WorkerPublishUtf8ArtifactPayload(
            request,
            root,
            L"job-result-" + jobId,
            L"job-result",
            payloadUtf8,
            manifestFields,
            config.artifactStore,
            config.artifactIdGenerator);

        return OkBaseLocal(L"publish_job_result_artifact", config) +
            L",\"schema_version\":" + JsonString(config.jobResultArtifactSchemaVersion) +
            L",\"artifact_schema\":" + JsonString(config.artifactUploadSchemaVersion) +
            L",\"artifact_id\":" + JsonString(published.artifactId) +
            L",\"artifact_kind\":" + JsonString(published.artifactKind) +
            L",\"status\":\"committed\"" +
            L",\"job_id\":" + JsonString(jobId) +
            L",\"job_status\":" + JsonString(record.status) +
            L",\"completed\":" + BoolJsonLocal(record.completed) +
            L",\"source_persisted\":" + BoolJsonLocal(record.persisted) +
            L",\"source_async_schema\":" + JsonString(record.sourceSchema) +
            L",\"source_snapshot_persisted\":" + BoolJsonLocal(record.snapshotPersisted) +
            L",\"source_snapshot_persist_error\":" + JsonString(record.snapshotPersistError) +
            L",\"source_result_bytes\":" + std::to_wstring(sourceResultBytes) +
            L",\"bytes\":" + std::to_wstring(published.bytes) +
            L",\"sha256\":" + JsonString(published.sha256) +
            L",\"blob_handle\":" + JsonString(published.blobHandle) +
            L",\"manifest_path\":" + JsonString(published.manifestPath) +
            L",\"deduplicated\":" + BoolJsonLocal(published.deduplicated) +
            L",\"workspace_bytes_before\":" + std::to_wstring(published.workspaceBytesBefore) +
            L",\"workspace_bytes_after\":" + std::to_wstring(published.workspaceBytesAfter) +
            L",\"workspace_budget_bytes\":" + std::to_wstring(published.workspaceBudgetBytes) +
            L",\"rollback_headroom_bytes\":" + std::to_wstring(published.rollbackHeadroomBytes) +
            L",\"storage_available_known\":" + BoolJsonLocal(published.storageAvailableKnown) +
            L",\"storage_available_bytes\":" + std::to_wstring(published.storageAvailableBytes) +
            L",\"publish_ms\":" + DoubleJsonLocal(published.publishMs, 6) +
            L"}";
    }
}
