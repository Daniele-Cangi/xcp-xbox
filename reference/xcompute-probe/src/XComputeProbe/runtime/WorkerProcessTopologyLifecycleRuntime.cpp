#include "pch.h"
#include "WorkerProcessTopologyLifecycleRuntime.h"

#include "WorkerProcessTopologyTransport.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe::ProcessTopology
{
    WorkerProcessTopologyLifecycleRuntime::WorkerProcessTopologyLifecycleRuntime()
        : journal_(L"foreground")
    {
    }

    bool WorkerProcessTopologyLifecycleRuntime::IsPhaseRequestSchema(
        std::wstring_view schemaVersion) noexcept
    {
        return schemaVersion == PhaseRequestSchema ||
            schemaVersion == PhaseRequestSchemaV1;
    }

    bool WorkerProcessTopologyLifecycleRuntime::IsLifecyclePhase(
        std::wstring_view phaseId) noexcept
    {
        return phaseId == L"foreground_suspend_resume" ||
            phaseId == L"foreground_terminate_restart" ||
            phaseId == L"auxiliary_terminate_restart" ||
            phaseId == L"package_redeploy";
    }

    std::wstring WorkerProcessTopologyLifecycleRuntime::ValidateLifecycleStage(
        JsonObject const& request,
        std::wstring_view requestSchema,
        BrokerPhaseBinding const& phaseBinding) const
    {
        if (requestSchema == PhaseRequestSchemaV1)
        {
            return L"none";
        }
        if (requestSchema != PhaseRequestSchema)
        {
            Fail(
                L"topology.request_schema_invalid",
                L"the lifecycle request schema is unsupported",
                std::wstring(requestSchema));
        }
        auto stage = RequiredString(
            request,
            L"lifecycle_stage",
            L"topology.request_schema_invalid");
        auto lifecyclePhase = IsLifecyclePhase(phaseBinding.phaseId);
        if ((lifecyclePhase && stage != L"prepare" && stage != L"resume") ||
            (!lifecyclePhase && stage != L"none"))
        {
            Fail(
                L"topology.request_schema_invalid",
                L"lifecycle_stage is not phase-exact",
                stage);
        }
        return stage;
    }

    void WorkerProcessTopologyLifecycleRuntime::RecordLifecycleEvent(
        std::wstring_view eventName) noexcept
    {
        journal_.Record(eventName);
    }

    void WorkerProcessTopologyLifecycleRuntime::RecordResumeIfNeeded(
        bool phaseInstrumented,
        std::wstring_view lifecycleStage,
        BrokerPhaseBinding const& phaseBinding) noexcept
    {
        if (phaseInstrumented && lifecycleStage == L"resume" &&
            phaseBinding.phaseId == L"foreground_suspend_resume")
        {
            journal_.Record(L"resumed");
        }
    }

    WorkerProcessTopologyLifecycleBrokerSnapshot
    WorkerProcessTopologyLifecycleRuntime::CaptureBrokerSnapshot(
        WorkerProcessTopologyTransport& transport,
        MonotonicDeadline const& deadline,
        std::wstring const& expectedActivationId) const
    {
        auto reply = SendBrokerCommand(
            transport,
            NewBrokerCommand(L"describe"),
            deadline.Remaining());
        if (reply.activationId != expectedActivationId)
        {
            Fail(
                L"topology.app_service_payload_invalid",
                L"the broker activation changed before final lifecycle snapshot");
        }
        return {
            RequiredObject(
                reply.result,
                L"lifecycle_journal",
                L"topology.app_service_payload_invalid"),
            reply.roundTripMilliseconds,
            reply.responseUtf8Bytes };
    }

    JsonObject WorkerProcessTopologyLifecycleRuntime::ForegroundComponent(
        std::wstring const& packageIdentity,
        uint64_t processId,
        JsonObject const& memory,
        JsonObject const& heartbeat,
        bool cpuSchedulingObserved) const
    {
        return ComponentProjection(
            L"foreground",
            L"foreground_compute",
            packageIdentity,
            L"pid:" + std::to_wstring(processId),
            journal_.ActivationIdentity(),
            L"foreground",
            memory,
            heartbeat,
            cpuSchedulingObserved);
    }

    JsonObject WorkerProcessTopologyLifecycleRuntime::BrokerComponent(
        std::wstring const& packageIdentity,
        uint64_t processId,
        std::wstring const& activationIdentity,
        JsonObject const& memory,
        JsonObject const& heartbeat,
        bool cpuSchedulingObserved)
    {
        return ComponentProjection(
            L"broker",
            L"auxiliary_probe",
            packageIdentity,
            L"pid:" + std::to_wstring(processId),
            activationIdentity,
            L"background",
            memory,
            heartbeat,
            cpuSchedulingObserved);
    }

    void WorkerProcessTopologyLifecycleRuntime::AttachForegroundJournal(
        JsonObject const& foreground) const
    {
        foreground.SetNamedValue(L"lifecycle_journal", journal_.Snapshot());
    }

    void WorkerProcessTopologyLifecycleRuntime::AttachBrokerJournal(
        JsonObject const& broker,
        JsonObject const& journal)
    {
        broker.SetNamedValue(L"lifecycle_journal", journal);
    }
}
