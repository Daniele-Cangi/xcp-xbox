#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace XComputeProbe
{
    struct WorkerDiagnosticsState
    {
        bool running = false;
        bool starting = false;
        uint16_t port = 0;
        uint64_t acceptedConnectionCount = 0;
        uint64_t completedRequestCount = 0;
        uint64_t failedRequestCount = 0;
        uint64_t activeSessionCount = 0;
        uint64_t activeAsyncJobCount = 0;
        uint64_t queuedAsyncJobCount = 0;
        bool asyncJobRunning = false;
        std::wstring latestAsyncJobId;
        std::wstring latestAsyncJobStatus;
        std::wstring latestAsyncJobError;
        uint64_t activeTaskPlanRunCount = 0;
        std::wstring activeTaskPlanId;
        std::wstring activeTaskPlanRunId;
        std::wstring latestTaskPlanId;
        std::wstring latestTaskPlanRunId;
        std::wstring latestTaskPlanRunStatus;
        std::wstring latestTaskPlanBlockedReason;
        std::wstring latestTaskPlanError;
        std::wstring lastError;
        std::vector<std::wstring> recentEvents;
    };

    struct WorkerDiagnosticsWorkspace
    {
        bool exists = false;
        uint64_t fileCount = 0;
        uint64_t directoryCount = 0;
        uint64_t byteCount = 0;
    };

    struct WorkerDiagnosticsInput
    {
        WorkerDiagnosticsState state;
        WorkerDiagnosticsWorkspace workspace;
    };

    std::wstring WorkerBuildDiagnosticsJson(WorkerDiagnosticsInput const& input);
}
