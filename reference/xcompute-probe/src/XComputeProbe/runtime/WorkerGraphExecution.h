#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "WorkerGraphPublishing.h"
#include "WorkerGraphRuntime.h"
#include "WorkerXvmRuntime.h"

namespace XComputeProbe
{
    struct WorkerGraphExecutionError : std::exception
    {
        std::string code;
        std::string message;

        WorkerGraphExecutionError(std::string codeValue, std::string messageValue);
        char const* what() const noexcept override;
    };

    struct WorkerGraphNodeExecutionInput
    {
        winrt::Windows::Data::Json::JsonObject node{ nullptr };
        std::wstring protocolVersion;
        std::wstring executionId;
        std::wstring nodeId;
        std::wstring command;
        std::wstring resultArtifactId;
        std::wstring inputBindingMode;
        std::wstring inputEdgesJson;
        std::wstring deterministicInputEdgesJson;
        std::wstring boundInputSha256;
        std::wstring boundInputMix64;
        WorkerXvmSnapshotAuthorityProvider snapshotAuthorityProvider;
        bool inputEdgesAllOk = true;
        bool inputControlPass = true;
        uint64_t inputEdgeCount = 0;
        WorkerGpuXvmExecutionPlan admittedGpuExecutionPlan;
        bool hasAdmittedGpuExecutionPlan = false;
        WorkerXvmSpmdExecutionPlan admittedSpmdExecutionPlan;
        bool hasAdmittedSpmdExecutionPlan = false;
        WorkerXvmCpuExecutionPlan admittedCpuExecutionPlan;
        bool hasAdmittedCpuExecutionPlan = false;
        WorkerXvmCpuCapsulePlan admittedCpuCapsulePlan;
        bool hasAdmittedCpuCapsulePlan = false;
        WorkerCpuGpuConvergencePlan admittedBackendConvergencePlan;
        bool hasAdmittedBackendConvergencePlan = false;
        WorkerVerifiedProductionModePlan admittedProductionModePlan;
        bool hasAdmittedProductionModePlan = false;
        double admittedCpuPlanBuildElapsedMs = 0.0;
    };

    struct WorkerGraphNodeExecutionResult
    {
        WorkerGraphNodeRecord record;
        std::wstring nodeResultJson;
        std::wstring canonicalNodeResultJson;
        bool ok = false;
        bool verified = false;
        uint64_t logicalBytesRead = 0;
    };

    using WorkerGraphComputeNodeRunner = std::function<std::wstring(
        winrt::Windows::Data::Json::JsonObject const& nodeRequest)>;

    using WorkerGraphWideMix64 = std::function<uint64_t(
        std::wstring const& text,
        uint64_t seed)>;

    WorkerGraphNodeExecutionResult WorkerGraphExecuteNode(
        WorkerGraphNodeExecutionInput const& input,
        WorkerGraphComputeNodeRunner const& computeRunner,
        WorkerXvmProgramResolver const& xvmResolver,
        WorkerXvmProgramTextReader const& xvmTextReader,
        WorkerGraphArtifactPublisher const& artifactPublisher,
        WorkerGraphSha256Text const& sha256Text,
        WorkerGraphWideMix64 const& wideMix64,
        std::function<bool()> const& cancelRequested = {});
}
