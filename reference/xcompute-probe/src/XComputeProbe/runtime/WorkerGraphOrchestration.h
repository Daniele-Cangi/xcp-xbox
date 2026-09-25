#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphCheckpointing.h"
#include "WorkerGraphExecution.h"
#include "WorkerGraphPublishing.h"
#include "WorkerXvmRuntime.h"

namespace XComputeProbe
{
    struct WorkerGraphOrchestrationServices
    {
        std::wstring protocolVersion;
        std::function<std::wstring()> generateSessionId;
        WorkerGraphComputeNodeRunner computeRunner;
        WorkerXvmProgramResolver xvmResolver;
        WorkerXvmProgramTextReader xvmTextReader;
        WorkerGraphArtifactPublisher artifactPublisher;
        WorkerGraphSha256Text sha256Text;
        WorkerGraphWideMix64 wideMix64;
        WorkerXvmSnapshotAuthorityProvider xvmSnapshotAuthorityProvider;
        WorkerXvmSnapshotAuthorityProvider xvmSnapshotAuthorityLoader;
    };

    using WorkerGraphCancelRequested = std::function<bool()>;

    std::wstring WorkerGraphExecuteSubmitGraph(
        winrt::Windows::Data::Json::JsonObject const& request,
        WorkerGraphOrchestrationServices const& services,
        WorkerGraphCancelRequested const& cancelRequested = {},
        WorkerGraphCheckpointObserver const& checkpointObserver = {});
}
