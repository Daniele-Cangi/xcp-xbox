#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "../ProbeResult.h"
#include "WorkerAsyncJobRuntime.h"
#include "WorkerCpuCapsuleRuntime.h"
#include "WorkerCreativeForegroundRuntime.h"
#include "WorkerPersistentComputeCoordinatorRuntime.h"
#include "WorkerProtocolBoundary.h"
#include "WorkerProcessTopologyRuntime.h"
#include "WorkerSessionRuntime.h"
#include "WorkerTaskPlanRuntime.h"

namespace XComputeProbe
{
    class WorkerCommandServer
    {
    public:
        WorkerCommandServer();
        bool Start(uint16_t port = 8787);
        void Stop();
        bool Restart();
        bool IsRunning() const { return m_running; }
        uint16_t Port() const { return m_port; }
        std::wstring PairingCode() const { return m_sessionRuntime.PairingCode(); }
        WorkerCreativeForegroundRuntime& CreativeForeground() noexcept
        {
            return m_creativeForegroundRuntime;
        }
        void RecordProcessTopologyLifecycleEvent(std::wstring_view eventName) noexcept
        {
            m_processTopologyRuntime.RecordLifecycleEvent(eventName);
        }

        struct StatusSnapshot
        {
            bool running = false;
            bool starting = false;
            uint16_t port = 0;
            uint64_t acceptedConnectionCount = 0;
            uint64_t completedRequestCount = 0;
            uint64_t failedRequestCount = 0;
            uint64_t activeSessionCount = 0;
            uint64_t trustedControllerCount = 0;
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

        StatusSnapshot Snapshot() const;

    private:
        winrt::fire_and_forget BindAsync(uint16_t port);
        void OnConnectionReceived(
            winrt::Windows::Networking::Sockets::StreamSocketListener const& sender,
            winrt::Windows::Networking::Sockets::StreamSocketListenerConnectionReceivedEventArgs const& args);
        winrt::fire_and_forget HandleClientAsync(winrt::Windows::Networking::Sockets::StreamSocket socket);
        std::wstring ProcessCommand(std::wstring const& requestJson);
        std::wstring ProcessBinaryCommand(winrt::Windows::Data::Json::JsonObject const& request, std::vector<uint8_t> const& data);
        std::wstring ExecuteCommand(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring OpenSessionWithTrust(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RegisterTrustedController(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring ListTrustedControllers(std::filesystem::path const& root);
        std::wstring RemoveTrustedController(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring DescribeRuntime(std::filesystem::path const& root);
        std::wstring SubmitStoredMemoryFileJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring SubmitGraphJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring SubmitD3D11ResidentHotLoopJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring SubmitD3D12ShaderShapeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring SubmitD3D12ShaderShapeSoakJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring GetJobStatus(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring GetJobResult(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);

        std::wstring CancelJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring PurgeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring ListJobs(std::filesystem::path const& root);
        std::wstring ExportDiagnostics(std::filesystem::path const& root);
        std::wstring ReadDiagnostics(std::filesystem::path const& root);
        std::wstring RunD3D11ComputeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11ComputeSweepJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11ComputeTimingJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11Fp32TimingJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11ShaderMatrixJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11ShaderMatrixStatsJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11SustainedSoakJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D11ResidentHotLoopJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12DeviceProbeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12ComputeSmokeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12ComputeSweepJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12ComputeTimingJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12Fp32TimingJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring RunD3D12ShaderShapeJob(winrt::Windows::Data::Json::JsonObject const& request, std::filesystem::path const& root);
        std::wstring ExecuteTaskPlanCommand(
            std::wstring const& command,
            winrt::Windows::Data::Json::JsonObject const& request,
            std::filesystem::path const& root);
        std::wstring ExecuteTaskPlanAction(
            std::wstring const& action,
            winrt::Windows::Data::Json::JsonObject const& step,
            std::filesystem::path const& root);
        void TryStartNextJob(std::filesystem::path const& root);
        void DispatchAsyncJob(std::shared_ptr<WorkerAsyncJobState> const& job, std::filesystem::path root);
        void RunAsyncStoredMemoryFileJob(std::shared_ptr<WorkerAsyncJobState> job, std::filesystem::path root);
        void RunAsyncGraphJob(std::shared_ptr<WorkerAsyncJobState> job, std::filesystem::path root);
        void RunAsyncD3D11ResidentHotLoopJob(std::shared_ptr<WorkerAsyncJobState> job, std::filesystem::path root);
        void RunAsyncD3D12ShaderShapeJob(std::shared_ptr<WorkerAsyncJobState> job, std::filesystem::path root);
        void RunAsyncD3D12ShaderShapeSoakJob(std::shared_ptr<WorkerAsyncJobState> job, std::filesystem::path root);
        void RecordEvent(std::wstring const& eventText);
        void RecordError(std::wstring const& errorText);
        void RecordCommandResult(std::wstring const& command, bool ok);

        winrt::Windows::Networking::Sockets::StreamSocketListener m_listener{ nullptr };
        winrt::event_token m_connectionToken{};
        WorkerSessionRuntime m_sessionRuntime;
        WorkerProtocolBoundary m_protocolBoundary;
        WorkerAsyncJobRuntime m_asyncJobRuntime;
        WorkerTaskPlanRuntime m_taskPlanRuntime;
        WorkerCreativeForegroundRuntime m_creativeForegroundRuntime;
        WorkerProcessTopologyRuntime m_processTopologyRuntime;
        WorkerPersistentComputeCoordinatorRuntime
            m_persistentComputeCoordinatorRuntime;
        WorkerCpuCapsuleRuntime m_cpuCapsuleRuntime;
        bool m_running = false;
        bool m_starting = false;
        uint16_t m_port = 0;
        uint64_t m_acceptedConnectionCount = 0;
        uint64_t m_completedRequestCount = 0;
        uint64_t m_failedRequestCount = 0;
        std::wstring m_lastError;
        std::vector<std::wstring> m_recentEvents;
        mutable std::mutex m_statusMutex;
    };
}
