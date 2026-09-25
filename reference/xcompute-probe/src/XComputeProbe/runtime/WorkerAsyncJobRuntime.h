#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

namespace XComputeProbe
{
    struct WorkerAsyncJobState
    {
        std::wstring jobId;
        std::wstring status;
        std::wstring kind;
        std::wstring programId;
        std::wstring inputPath;
        std::wstring outputPath;
        std::wstring variant;
        std::wstring elementsList;
        std::wstring shaderVariants;
        std::wstring graphRequestJson;
        std::wstring graphId;
        std::wstring graphLastCheckpointArtifactId;
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;
        bool snapshotPersisted = false;
        std::wstring snapshotPersistError;
        uint64_t elements = 0;
        uint64_t durationSeconds = 0;
        uint64_t windowSeconds = 0;
        uint64_t dispatchesPerSample = 0;
        uint64_t warmupDispatches = 0;
        uint64_t maxDispatches = 0;
        uint64_t repeats = 0;
        uint64_t warmupRepeats = 0;
        uint64_t windowCount = 0;
        uint64_t discardInitialWindows = 0;
        uint64_t windowPauseMs = 0;
        uint64_t iterations = 0;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t queueOrdinal = 0;
        uint64_t graphNodeCount = 0;
        uint64_t graphCheckpointCount = 0;
        std::chrono::steady_clock::time_point submittedAt;
        std::chrono::steady_clock::time_point startedAt;
        std::chrono::steady_clock::time_point endedAt;
        std::atomic_bool cancelRequested{ false };
    };

    struct WorkerAsyncJobSubmissionResult
    {
        bool ok = false;
        std::shared_ptr<WorkerAsyncJobState> job;
        size_t queueDepth = 0;
        std::wstring errorCode;
        std::wstring errorMessage;
    };

    struct WorkerAsyncJobQueueObservation
    {
        std::wstring status;
        size_t queueDepth = 0;
    };

    struct WorkerAsyncJobRecord
    {
        bool ok = false;
        bool persisted = false;
        bool snapshotPersisted = false;
        bool completed = false;
        bool resultAvailable = false;
        std::wstring sourceSchema;
        std::wstring jobId;
        std::wstring status;
        std::wstring jobStatusJson;
        std::wstring resultJson;
        std::wstring errorCode;
        std::wstring errorMessage;
        std::wstring snapshotPersistError;
        std::wstring operationErrorCode;
        std::wstring operationErrorMessage;
    };

    struct WorkerAsyncJobCancelResult
    {
        bool ok = false;
        std::wstring jobId;
        std::wstring status;
        bool cancelRequested = false;
        bool cancelAccepted = false;
        bool snapshotPersisted = false;
        std::wstring errorCode;
        std::wstring errorMessage;
    };

    struct WorkerAsyncJobPurgeResult
    {
        bool ok = false;
        std::wstring jobId;
        bool memoryExisted = false;
        bool queueRemoved = false;
        bool persistedExisted = false;
        bool preserveSnapshot = false;
        bool persistedRemoved = false;
        bool purged = false;
        std::wstring errorCode;
        std::wstring errorMessage;
    };

    struct WorkerAsyncJobListResult
    {
        std::wstring jobsJson = L"[]";
        size_t jobCount = 0;
        size_t memoryJobCount = 0;
        size_t persistedJobCount = 0;
    };

    struct WorkerAsyncJobRuntimeStatus
    {
        size_t activeJobCount = 0;
        size_t queuedJobCount = 0;
        bool jobRunning = false;
        std::wstring latestJobId;
        std::wstring latestJobStatus;
        std::wstring latestJobError;
    };

    using WorkerAsyncJobExecutor = std::function<void(std::shared_ptr<WorkerAsyncJobState> const&)>;
    using WorkerAsyncJobEventSink = std::function<void(std::wstring const&)>;

    class WorkerAsyncJobRuntime
    {
    public:
        static constexpr uint64_t MaxJobs = 64;

        WorkerAsyncJobRuntime(
            std::wstring protocolVersion,
            WorkerAsyncJobEventSink eventSink = {},
            WorkerAsyncJobEventSink errorSink = {});

        static wchar_t const* SchemaVersion();
        static bool IsTerminalStatus(std::wstring const& status);

        std::filesystem::path SnapshotDirectory(std::filesystem::path const& root) const;

        WorkerAsyncJobSubmissionResult Enqueue(
            std::filesystem::path const& root,
            std::shared_ptr<WorkerAsyncJobState> const& job);
        WorkerAsyncJobQueueObservation Observe(
            std::shared_ptr<WorkerAsyncJobState> const& job) const;
        bool TryStartNext(WorkerAsyncJobExecutor const& executor);

        void Complete(
            std::filesystem::path const& root,
            std::shared_ptr<WorkerAsyncJobState> const& job,
            std::wstring const& status,
            std::wstring resultJson,
            std::wstring errorCode,
            std::wstring errorMessage);
        void UpdateGraphCheckpoint(
            std::shared_ptr<WorkerAsyncJobState> const& job,
            uint64_t checkpointCount,
            std::wstring const& checkpointArtifactId);

        WorkerAsyncJobRecord Read(
            std::filesystem::path const& root,
            std::wstring const& jobId,
            bool persistTerminal);
        WorkerAsyncJobCancelResult RequestCancel(
            std::filesystem::path const& root,
            std::wstring const& jobId);
        WorkerAsyncJobPurgeResult Purge(
            std::filesystem::path const& root,
            std::wstring const& jobId,
            bool preserveSnapshot);
        WorkerAsyncJobListResult List(std::filesystem::path const& root) const;
        WorkerAsyncJobRuntimeStatus Snapshot() const;

    private:
        static bool ValidateJobId(std::wstring const& jobId);
        static std::wstring BoolJson(bool value);
        static std::wstring OptionalString(
            winrt::Windows::Data::Json::JsonObject const& object,
            wchar_t const* name,
            std::wstring const& fallback = L"");

        std::filesystem::path ResolveSnapshotPath(
            std::filesystem::path const& root,
            std::wstring const& jobId) const;
        bool TryReadPersistedSnapshot(
            std::filesystem::path const& root,
            std::wstring const& jobId,
            winrt::Windows::Data::Json::JsonObject& snapshot) const;
        bool PersistSnapshot(
            std::filesystem::path const& root,
            std::shared_ptr<WorkerAsyncJobState> const& job);
        std::wstring SummaryJsonLocked(WorkerAsyncJobState const& job) const;
        std::wstring SnapshotJsonLocked(WorkerAsyncJobState const& job) const;
        WorkerAsyncJobRecord RecordLocked(WorkerAsyncJobState const& job) const;
        bool TransitionLocked(
            WorkerAsyncJobState& job,
            std::wstring const& nextStatus);
        void PruneCompletedLocked();
        uint64_t AllocateOrdinalLocked(std::filesystem::path const& root);

        std::wstring protocolVersion_;
        WorkerAsyncJobEventSink eventSink_;
        WorkerAsyncJobEventSink errorSink_;
        std::map<std::wstring, std::shared_ptr<WorkerAsyncJobState>> jobs_;
        std::vector<std::wstring> queue_;
        mutable std::mutex mutex_;
        bool jobRunning_ = false;
        std::wstring runningJobId_;
        uint64_t nextOrdinal_ = 1;
    };
}

