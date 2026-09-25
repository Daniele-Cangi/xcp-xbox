#pragma once

#include <cstdint>
#include <string>

namespace XComputeProbe
{
    struct WorkerRuntimeDescriptionLimits
    {
        uint64_t maxRequestBytes = 0;
        uint64_t binaryPayloadReadBufferBytes = 0;
        uint64_t requestReadBufferBytes = 0;
        uint64_t defaultSessionTtlSeconds = 0;
        uint64_t minSessionTtlSeconds = 0;
        uint64_t maxSessionTtlSeconds = 0;
        uint64_t maxTrustedControllers = 0;
        uint64_t maxArtifactJsonChunkBytes = 0;
        uint64_t maxArtifactManifestJsonBytes = 0;
        uint64_t artifactManifestJobReadBufferBytes = 0;
        uint64_t maxStreamReadBytes = 0;
        uint64_t maxStreamWriteBytes = 0;
        uint64_t maxArtifactManifestParts = 0;
        uint64_t maxArtifactExpectedBytes = 0;
        uint64_t defaultArtifactRollbackHeadroomBytes = 0;
        uint64_t defaultArtifactWorkspaceBudgetBytes = 0;
        uint64_t maxArtifactWorkspaceBudgetBytes = 0;
        uint64_t maxAsyncJobs = 0;
        uint64_t maxPhysicsGridWidth = 0;
        uint64_t maxPhysicsGridHeight = 0;
        uint64_t maxPhysicsCellCount = 0;
        uint64_t maxPhysicsSteps = 0;
        uint64_t maxPhysicsCellUpdates = 0;
        uint64_t maxD3D12Fp32TimingElements = 0;
        uint64_t maxD3D12ShaderShapeElements = 0;
    };

    struct WorkerRuntimeDescriptionState
    {
        bool running = false;
        bool starting = false;
        bool asyncJobRunning = false;
        uint64_t activeSessionCount = 0;
        uint64_t activeAsyncJobCount = 0;
        uint64_t queuedAsyncJobCount = 0;
        uint64_t persistedJobCount = 0;
        uint64_t taskPlanCount = 0;
        uint64_t activeTaskPlanRunCount = 0;
        std::wstring latestAsyncJobId;
        std::wstring latestAsyncJobStatus;
        std::wstring latestTaskPlanId;
        std::wstring latestTaskPlanRunStatus;
    };

    struct WorkerRuntimeDescriptionInput
    {
        std::wstring protocolVersion;
        uint16_t port = 0;
        WorkerRuntimeDescriptionLimits limits;
        WorkerRuntimeDescriptionState state;
    };

    std::wstring WorkerBuildRuntimeDescription(WorkerRuntimeDescriptionInput const& input);
}
