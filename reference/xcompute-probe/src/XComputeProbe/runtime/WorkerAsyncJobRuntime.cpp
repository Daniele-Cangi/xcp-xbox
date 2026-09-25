#include "pch.h"

#include "WorkerAsyncJobRuntime.h"

#include "../ProbeResult.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <thread>

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace fs = std::filesystem;

    namespace
    {
        struct WorkerAsyncJobStateTransition
        {
            wchar_t const* from;
            wchar_t const* to;
        };

        constexpr std::array<WorkerAsyncJobStateTransition, 6> WorkerAsyncJobStateTransitions{ {
            { L"", L"queued" },
            { L"queued", L"running" },
            { L"queued", L"canceled" },
            { L"running", L"succeeded" },
            { L"running", L"failed" },
            { L"running", L"canceled" },
        } };

        std::wstring Utf8ToWide(std::string const& value)
        {
            return winrt::to_hstring(value).c_str();
        }

        std::string WideToUtf8(std::wstring const& value)
        {
            return winrt::to_string(winrt::hstring(value));
        }

        std::string ReadTextFile(fs::path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("could not open async job snapshot for read");
            }
            return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        }

        void WriteTextFile(fs::path const& path, std::string const& content)
        {
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error("could not open async job snapshot for write");
            }
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.flush();
            if (!output.good())
            {
                throw std::runtime_error("async job snapshot write did not complete");
            }
        }
    }

    WorkerAsyncJobRuntime::WorkerAsyncJobRuntime(
        std::wstring protocolVersion,
        WorkerAsyncJobEventSink eventSink,
        WorkerAsyncJobEventSink errorSink)
        : protocolVersion_(std::move(protocolVersion)),
          eventSink_(std::move(eventSink)),
          errorSink_(std::move(errorSink))
    {
    }

    wchar_t const* WorkerAsyncJobRuntime::SchemaVersion()
    {
        return L"worker-async-job-0.18";
    }

    bool WorkerAsyncJobRuntime::IsTerminalStatus(std::wstring const& status)
    {
        return status == L"succeeded" || status == L"failed" || status == L"canceled";
    }

    std::wstring WorkerAsyncJobRuntime::BoolJson(bool value)
    {
        return value ? L"true" : L"false";
    }

    std::wstring WorkerAsyncJobRuntime::OptionalString(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring const& fallback)
    {
        if (!object || !object.HasKey(name))
        {
            return fallback;
        }
        try
        {
            return object.GetNamedString(name).c_str();
        }
        catch (...)
        {
            return fallback;
        }
    }

    bool WorkerAsyncJobRuntime::ValidateJobId(std::wstring const& jobId)
    {
        if (jobId.empty() || jobId.size() > 64)
        {
            return false;
        }
        return std::all_of(jobId.begin(), jobId.end(), [](wchar_t value)
        {
            return (value >= L'a' && value <= L'z') ||
                (value >= L'A' && value <= L'Z') ||
                (value >= L'0' && value <= L'9') ||
                value == L'_' ||
                value == L'-';
        });
    }

    fs::path WorkerAsyncJobRuntime::SnapshotDirectory(fs::path const& root) const
    {
        return root / L"jobs" / L"async";
    }

    fs::path WorkerAsyncJobRuntime::ResolveSnapshotPath(
        fs::path const& root,
        std::wstring const& jobId) const
    {
        return SnapshotDirectory(root) / (jobId + L".json");
    }

    bool WorkerAsyncJobRuntime::TryReadPersistedSnapshot(
        fs::path const& root,
        std::wstring const& jobId,
        JsonObject& snapshot) const
    {
        auto path = ResolveSnapshotPath(root, jobId);
        if (!fs::is_regular_file(path))
        {
            return false;
        }

        try
        {
            snapshot = JsonObject::Parse(Utf8ToWide(ReadTextFile(path)));
            return snapshot.HasKey(L"schema_version") && snapshot.HasKey(L"job_status");
        }
        catch (...)
        {
            return false;
        }
    }

    bool WorkerAsyncJobRuntime::TransitionLocked(
        WorkerAsyncJobState& job,
        std::wstring const& nextStatus)
    {
        if (IsTerminalStatus(job.status))
        {
            return false;
        }

        auto transition = std::find_if(
            WorkerAsyncJobStateTransitions.begin(),
            WorkerAsyncJobStateTransitions.end(),
            [&job, &nextStatus](WorkerAsyncJobStateTransition const& candidate)
            {
                return job.status == candidate.from && nextStatus == candidate.to;
            });
        if (transition == WorkerAsyncJobStateTransitions.end())
        {
            return false;
        }

        job.status = nextStatus;
        return true;
    }

    uint64_t WorkerAsyncJobRuntime::AllocateOrdinalLocked(fs::path const& root)
    {
        for (uint64_t attempt = 0; attempt < 1000000; ++attempt)
        {
            auto ordinal = nextOrdinal_++;
            auto candidate = L"job-" + std::to_wstring(ordinal);
            if (jobs_.find(candidate) != jobs_.end())
            {
                continue;
            }
            if (fs::exists(ResolveSnapshotPath(root, candidate)))
            {
                continue;
            }
            return ordinal;
        }
        return 0;
    }

    void WorkerAsyncJobRuntime::PruneCompletedLocked()
    {
        while (jobs_.size() >= MaxJobs)
        {
            auto eraseIt = jobs_.end();
            uint64_t oldestOrdinal = UINT64_MAX;
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
            {
                if (IsTerminalStatus(it->second->status) &&
                    it->second->queueOrdinal < oldestOrdinal)
                {
                    oldestOrdinal = it->second->queueOrdinal;
                    eraseIt = it;
                }
            }
            if (eraseIt == jobs_.end())
            {
                break;
            }
            jobs_.erase(eraseIt);
        }
    }

    WorkerAsyncJobSubmissionResult WorkerAsyncJobRuntime::Enqueue(
        fs::path const& root,
        std::shared_ptr<WorkerAsyncJobState> const& job)
    {
        WorkerAsyncJobSubmissionResult result;
        if (!job)
        {
            result.errorCode = L"job.invalid";
            result.errorMessage = L"async job state is required";
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        PruneCompletedLocked();
        if (jobs_.size() >= MaxJobs)
        {
            result.errorCode = L"job_queue.full";
            result.errorMessage = L"async job queue is full";
            return result;
        }

        auto ordinal = AllocateOrdinalLocked(root);
        if (ordinal == 0)
        {
            result.errorCode = L"job.id_exhausted";
            result.errorMessage = L"could not allocate a unique async job id";
            return result;
        }

        job->jobId = L"job-" + std::to_wstring(ordinal);
        job->queueOrdinal = ordinal;
        job->status.clear();
        job->submittedAt = std::chrono::steady_clock::now();
        job->snapshotPersisted = false;
        job->snapshotPersistError.clear();
        if (!TransitionLocked(*job, L"queued"))
        {
            result.errorCode = L"job.state_transition_invalid";
            result.errorMessage = L"async job could not enter queued state";
            return result;
        }

        jobs_[job->jobId] = job;
        queue_.push_back(job->jobId);
        result.ok = true;
        result.job = job;
        result.queueDepth = queue_.size();
        return result;
    }

    WorkerAsyncJobQueueObservation WorkerAsyncJobRuntime::Observe(
        std::shared_ptr<WorkerAsyncJobState> const& job) const
    {
        WorkerAsyncJobQueueObservation result;
        std::lock_guard<std::mutex> lock(mutex_);
        if (job)
        {
            result.status = job->status;
        }
        result.queueDepth = queue_.size();
        return result;
    }

    bool WorkerAsyncJobRuntime::TryStartNext(WorkerAsyncJobExecutor const& executor)
    {
        if (!executor)
        {
            return false;
        }

        std::shared_ptr<WorkerAsyncJobState> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobRunning_)
            {
                return false;
            }

            while (!queue_.empty())
            {
                auto jobId = queue_.front();
                queue_.erase(queue_.begin());
                auto found = jobs_.find(jobId);
                if (found == jobs_.end() || found->second->status != L"queued")
                {
                    continue;
                }

                if (!TransitionLocked(*found->second, L"running"))
                {
                    if (errorSink_)
                    {
                        errorSink_(L"async job state transition rejected: " + jobId + L" queued -> running");
                    }
                    continue;
                }

                job = found->second;
                job->startedAt = std::chrono::steady_clock::now();
                jobRunning_ = true;
                runningJobId_ = jobId;
                break;
            }
        }

        if (!job)
        {
            return false;
        }

        std::thread([executor, job]()
        {
            executor(job);
        }).detach();
        return true;
    }

    void WorkerAsyncJobRuntime::Complete(
        fs::path const& root,
        std::shared_ptr<WorkerAsyncJobState> const& job,
        std::wstring const& status,
        std::wstring resultJson,
        std::wstring errorCode,
        std::wstring errorMessage)
    {
        bool completed = false;
        std::wstring jobId;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!job)
            {
                return;
            }

            jobId = job->jobId;
            auto found = jobs_.find(jobId);
            if (found == jobs_.end() || found->second.get() != job.get())
            {
                if (errorSink_)
                {
                    errorSink_(L"async job completion rejected for unknown state: " + jobId);
                }
                return;
            }

            if (!TransitionLocked(*job, status))
            {
                if (errorSink_)
                {
                    errorSink_(L"async job state transition rejected: " + jobId + L" " + job->status + L" -> " + status);
                }
                return;
            }

            job->resultJson = std::move(resultJson);
            job->errorCode = std::move(errorCode);
            job->errorMessage = std::move(errorMessage);
            job->endedAt = std::chrono::steady_clock::now();
            if (runningJobId_ == jobId)
            {
                jobRunning_ = false;
                runningJobId_.clear();
            }
            completed = true;
        }

        if (completed)
        {
            PersistSnapshot(root, job);
        }
    }

    void WorkerAsyncJobRuntime::UpdateGraphCheckpoint(
        std::shared_ptr<WorkerAsyncJobState> const& job,
        uint64_t checkpointCount,
        std::wstring const& checkpointArtifactId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!job || IsTerminalStatus(job->status))
        {
            return;
        }
        job->graphCheckpointCount = checkpointCount;
        job->graphLastCheckpointArtifactId = checkpointArtifactId;
    }

    std::wstring WorkerAsyncJobRuntime::SummaryJsonLocked(
        WorkerAsyncJobState const& job) const
    {
        auto completed = IsTerminalStatus(job.status);
        auto kind = job.kind.empty() ? L"async_memory_file_stored" : job.kind;
        std::wstring summary = L"{\"job_id\":" + JsonString(job.jobId) +
            L",\"status\":" + JsonString(job.status) +
            L",\"job\":" + JsonString(kind) +
            L",\"job_kind\":" + JsonString(kind) +
            L",\"program_id\":" + JsonString(job.programId) +
            L",\"input_path\":" + JsonString(job.inputPath) +
            L",\"output_path\":" + JsonString(job.outputPath) +
            L",\"iterations\":" + std::to_wstring(job.iterations) +
            L",\"memory_bytes\":" + std::to_wstring(job.memoryBytes) +
            L",\"output_bytes\":" + std::to_wstring(job.outputBytes) +
            L",\"queue_ordinal\":" + std::to_wstring(job.queueOrdinal) +
            L",\"cancel_requested\":" + BoolJson(job.cancelRequested.load()) +
            L",\"completed\":" + BoolJson(completed) +
            L",\"result_available\":" + BoolJson(!job.resultJson.empty()) +
            L",\"snapshot_persisted\":" + BoolJson(job.snapshotPersisted);
        if (!job.snapshotPersistError.empty())
        {
            summary += L",\"snapshot_persist_error\":" + JsonString(job.snapshotPersistError);
        }
        if (kind == L"async_d3d11_resident_hotloop")
        {
            summary += L",\"variant_id\":" + JsonString(job.variant) +
                L",\"elements\":" + std::to_wstring(job.elements) +
                L",\"duration_seconds\":" + std::to_wstring(job.durationSeconds) +
                L",\"window_seconds\":" + std::to_wstring(job.windowSeconds) +
                L",\"dispatches_per_sample\":" + std::to_wstring(job.dispatchesPerSample) +
                L",\"warmup_dispatches\":" + std::to_wstring(job.warmupDispatches) +
                L",\"max_dispatches\":" + std::to_wstring(job.maxDispatches);
        }
        if (kind == L"async_d3d12_shader_shape")
        {
            summary += L",\"elements_list\":" + JsonString(job.elementsList) +
                L",\"variants\":" + JsonString(job.shaderVariants) +
                L",\"repeats\":" + std::to_wstring(job.repeats) +
                L",\"warmup_repeats\":" + std::to_wstring(job.warmupRepeats);
        }
        if (kind == L"async_d3d12_shader_shape_soak")
        {
            summary += L",\"elements_list\":" + JsonString(job.elementsList) +
                L",\"variants\":" + JsonString(job.shaderVariants) +
                L",\"repeats\":" + std::to_wstring(job.repeats) +
                L",\"warmup_repeats\":" + std::to_wstring(job.warmupRepeats) +
                L",\"window_count\":" + std::to_wstring(job.windowCount) +
                L",\"discard_initial_windows\":" + std::to_wstring(job.discardInitialWindows) +
                L",\"window_pause_ms\":" + std::to_wstring(job.windowPauseMs);
        }
        if (kind == L"async_graph")
        {
            summary += L",\"graph_id\":" + JsonString(job.graphId) +
                L",\"graph_node_count\":" + std::to_wstring(job.graphNodeCount) +
                L",\"graph_checkpoint_count\":" + std::to_wstring(job.graphCheckpointCount) +
                L",\"graph_last_checkpoint_artifact_id\":" + JsonString(job.graphLastCheckpointArtifactId);
        }
        summary += L"}";
        return summary;
    }

    std::wstring WorkerAsyncJobRuntime::SnapshotJsonLocked(
        WorkerAsyncJobState const& job) const
    {
        std::wstring snapshot = L"{\"schema_version\":" + JsonString(SchemaVersion()) +
            L",\"protocol_version\":" + JsonString(protocolVersion_) +
            L",\"job_status\":" + SummaryJsonLocked(job);
        if (!job.resultJson.empty())
        {
            snapshot += L",\"result\":" + job.resultJson;
        }
        if (!job.errorCode.empty())
        {
            snapshot += L",\"error\":{\"code\":" + JsonString(job.errorCode) +
                L",\"message\":" + JsonString(job.errorMessage) + L"}";
        }
        snapshot += L"}";
        return snapshot;
    }

    bool WorkerAsyncJobRuntime::PersistSnapshot(
        fs::path const& root,
        std::shared_ptr<WorkerAsyncJobState> const& job)
    {
        std::wstring jobId;
        std::wstring snapshotJson;
        std::wstring errorText;
        try
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobId = job->jobId;
            snapshotJson = SnapshotJsonLocked(*job);
        }
        catch (std::exception const& ex)
        {
            errorText = Utf8ToWide(ex.what());
        }
        catch (...)
        {
            errorText = L"unhandled snapshot serialization failure";
        }

        if (errorText.empty())
        {
            try
            {
                WriteTextFile(ResolveSnapshotPath(root, jobId), WideToUtf8(snapshotJson));
            }
            catch (std::exception const& ex)
            {
                errorText = Utf8ToWide(ex.what());
            }
            catch (...)
            {
                errorText = L"unhandled snapshot write failure";
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(jobId);
            if (found != jobs_.end())
            {
                found->second->snapshotPersisted = errorText.empty();
                found->second->snapshotPersistError = errorText;
            }
        }

        if (!errorText.empty())
        {
            if (errorSink_)
            {
                errorSink_(L"async job snapshot write failed for " + jobId + L": " + errorText);
            }
            return false;
        }

        if (eventSink_)
        {
            eventSink_(L"async job snapshot persisted: " + jobId);
        }
        return true;
    }

    WorkerAsyncJobRecord WorkerAsyncJobRuntime::RecordLocked(
        WorkerAsyncJobState const& job) const
    {
        WorkerAsyncJobRecord record;
        record.ok = true;
        record.persisted = false;
        record.snapshotPersisted = job.snapshotPersisted;
        record.completed = IsTerminalStatus(job.status);
        record.resultAvailable = !job.resultJson.empty();
        record.sourceSchema = SchemaVersion();
        record.jobId = job.jobId;
        record.status = job.status;
        record.jobStatusJson = SummaryJsonLocked(job);
        record.resultJson = job.resultJson;
        record.errorCode = job.errorCode;
        record.errorMessage = job.errorMessage;
        record.snapshotPersistError = job.snapshotPersistError;
        return record;
    }

    WorkerAsyncJobRecord WorkerAsyncJobRuntime::Read(
        fs::path const& root,
        std::wstring const& jobId,
        bool persistTerminal)
    {
        WorkerAsyncJobRecord record;
        if (!ValidateJobId(jobId))
        {
            record.operationErrorCode = L"job_id.invalid";
            record.operationErrorMessage = L"job_id must be 1..64 chars using letters, digits, underscore, or dash";
            return record;
        }

        std::shared_ptr<WorkerAsyncJobState> memoryJob;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(jobId);
            if (found != jobs_.end())
            {
                memoryJob = found->second;
            }
        }

        if (memoryJob)
        {
            bool shouldPersist = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldPersist = persistTerminal &&
                    IsTerminalStatus(memoryJob->status) &&
                    !memoryJob->snapshotPersisted;
            }
            if (shouldPersist)
            {
                PersistSnapshot(root, memoryJob);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            return RecordLocked(*memoryJob);
        }

        JsonObject snapshot{ nullptr };
        if (!TryReadPersistedSnapshot(root, jobId, snapshot))
        {
            record.operationErrorCode = L"job.not_found";
            record.operationErrorMessage = L"async job was not found";
            return record;
        }

        try
        {
            auto jobStatus = snapshot.GetNamedObject(L"job_status");
            record.ok = true;
            record.persisted = true;
            record.snapshotPersisted = true;
            record.sourceSchema = OptionalString(snapshot, L"schema_version", SchemaVersion());
            record.jobId = OptionalString(jobStatus, L"job_id", jobId);
            record.status = OptionalString(jobStatus, L"status");
            record.completed = jobStatus.GetNamedBoolean(
                L"completed",
                IsTerminalStatus(record.status));
            record.jobStatusJson = std::wstring(jobStatus.Stringify().c_str());
            if (snapshot.HasKey(L"result"))
            {
                auto result = snapshot.GetNamedObject(L"result");
                record.resultJson = std::wstring(result.Stringify().c_str());
                record.resultAvailable = true;
            }
            if (snapshot.HasKey(L"error"))
            {
                auto error = snapshot.GetNamedObject(L"error");
                record.errorCode = OptionalString(error, L"code");
                record.errorMessage = OptionalString(error, L"message");
            }
            return record;
        }
        catch (...)
        {
            record = {};
            record.operationErrorCode = L"job.snapshot_invalid";
            record.operationErrorMessage = L"persisted async job snapshot is invalid";
            return record;
        }
    }

    WorkerAsyncJobCancelResult WorkerAsyncJobRuntime::RequestCancel(
        fs::path const& root,
        std::wstring const& jobId)
    {
        WorkerAsyncJobCancelResult result;
        result.jobId = jobId;
        if (!ValidateJobId(jobId))
        {
            result.errorCode = L"job_id.invalid";
            result.errorMessage = L"job_id must be 1..64 chars using letters, digits, underscore, or dash";
            return result;
        }

        std::shared_ptr<WorkerAsyncJobState> job;
        bool persistCanceled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(jobId);
            if (found == jobs_.end())
            {
                result.errorCode = L"job.not_found";
                result.errorMessage = L"async job was not found";
                return result;
            }

            job = found->second;
            if (job->status == L"queued")
            {
                job->cancelRequested.store(true);
                if (!TransitionLocked(*job, L"canceled"))
                {
                    result.errorCode = L"job.state_transition_invalid";
                    result.errorMessage = L"queued async job could not enter canceled state";
                    return result;
                }
                job->errorCode = L"job.canceled";
                job->errorMessage = L"job was canceled before start";
                job->endedAt = std::chrono::steady_clock::now();
                result.cancelAccepted = true;
                persistCanceled = true;
            }
            else if (job->status == L"running")
            {
                job->cancelRequested.store(true);
                result.cancelAccepted = true;
            }

            result.ok = true;
            result.status = job->status;
            result.cancelRequested = job->cancelRequested.load();
            result.snapshotPersisted = job->snapshotPersisted;
        }

        if (persistCanceled)
        {
            result.snapshotPersisted = PersistSnapshot(root, job);
        }
        return result;
    }

    WorkerAsyncJobPurgeResult WorkerAsyncJobRuntime::Purge(
        fs::path const& root,
        std::wstring const& jobId,
        bool preserveSnapshot)
    {
        WorkerAsyncJobPurgeResult result;
        result.jobId = jobId;
        result.preserveSnapshot = preserveSnapshot;
        if (!ValidateJobId(jobId))
        {
            result.errorCode = L"job_id.invalid";
            result.errorMessage = L"job_id must be 1..64 chars using letters, digits, underscore, or dash";
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(jobId);
            if (found != jobs_.end())
            {
                if (found->second->status == L"running")
                {
                    result.errorCode = L"job.running";
                    result.errorMessage = L"running jobs cannot be purged";
                    return result;
                }
                jobs_.erase(found);
                result.memoryExisted = true;
            }

            auto before = queue_.size();
            queue_.erase(std::remove(queue_.begin(), queue_.end(), jobId), queue_.end());
            result.queueRemoved = queue_.size() != before;
        }

        try
        {
            auto snapshotPath = ResolveSnapshotPath(root, jobId);
            if (fs::is_regular_file(snapshotPath))
            {
                result.persistedExisted = true;
                if (!preserveSnapshot)
                {
                    result.persistedRemoved = fs::remove(snapshotPath);
                }
            }
        }
        catch (std::exception const& ex)
        {
            result.errorCode = L"job.purge_failed";
            result.errorMessage = Utf8ToWide(ex.what());
            return result;
        }

        result.ok = true;
        result.purged = result.memoryExisted || result.persistedRemoved;
        return result;
    }

    WorkerAsyncJobListResult WorkerAsyncJobRuntime::List(fs::path const& root) const
    {
        WorkerAsyncJobListResult result;
        std::vector<std::wstring> entries;
        std::vector<std::wstring> memoryJobIds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto const& item : jobs_)
            {
                entries.push_back(SummaryJsonLocked(*item.second));
                memoryJobIds.push_back(item.first);
            }
        }

        auto snapshotDirectory = SnapshotDirectory(root);
        if (fs::exists(snapshotDirectory))
        {
            for (auto const& entry : fs::directory_iterator(snapshotDirectory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".json")
                {
                    continue;
                }

                try
                {
                    auto snapshot = JsonObject::Parse(Utf8ToWide(ReadTextFile(entry.path())));
                    if (!snapshot.HasKey(L"job_status"))
                    {
                        continue;
                    }
                    auto jobStatus = snapshot.GetNamedObject(L"job_status");
                    auto jobId = OptionalString(jobStatus, L"job_id");
                    if (jobId.empty() ||
                        std::find(memoryJobIds.begin(), memoryJobIds.end(), jobId) != memoryJobIds.end())
                    {
                        continue;
                    }
                    entries.push_back(std::wstring(jobStatus.Stringify().c_str()));
                    ++result.persistedJobCount;
                }
                catch (...)
                {
                }
            }
        }
        std::sort(entries.begin(), entries.end());

        std::wostringstream jobs;
        jobs << L"[";
        for (size_t index = 0; index < entries.size(); ++index)
        {
            if (index != 0)
            {
                jobs << L",";
            }
            jobs << entries[index];
        }
        jobs << L"]";

        result.jobsJson = jobs.str();
        result.jobCount = entries.size();
        result.memoryJobCount = memoryJobIds.size();
        return result;
    }

    WorkerAsyncJobRuntimeStatus WorkerAsyncJobRuntime::Snapshot() const
    {
        WorkerAsyncJobRuntimeStatus result;
        std::lock_guard<std::mutex> lock(mutex_);
        result.activeJobCount = jobs_.size();
        result.queuedJobCount = queue_.size();
        result.jobRunning = jobRunning_;

        std::shared_ptr<WorkerAsyncJobState> newestJob;
        for (auto const& item : jobs_)
        {
            if (!newestJob || item.second->queueOrdinal > newestJob->queueOrdinal)
            {
                newestJob = item.second;
            }
        }
        if (newestJob)
        {
            result.latestJobId = newestJob->jobId;
            result.latestJobStatus = newestJob->status;
            result.latestJobError = newestJob->errorCode.empty()
                ? newestJob->errorMessage
                : newestJob->errorCode;
        }
        return result;
    }
}

