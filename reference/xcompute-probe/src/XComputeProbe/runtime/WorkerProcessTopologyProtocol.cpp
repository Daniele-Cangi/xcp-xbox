#include "pch.h"
#include "WorkerProcessTopologyProtocol.h"
#include "WorkerProcessTopologyTransport.h"

#include "WorkerProcessTopologyRuntime.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <vector>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Json;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;

namespace XComputeProbe::ProcessTopology
{
    namespace
    {
        double ElapsedMilliseconds(
            std::chrono::steady_clock::time_point started)
        {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }

        std::wstring BrokerFailureCode(std::wstring const& command)
        {
            if (command == L"allocate_touch")
            {
                return L"topology.memory_allocation_failed";
            }
            if (command == L"heartbeat" || command == L"release")
            {
                return L"topology.memory_verification_failed";
            }
            if (command == L"storage_exchange" ||
                command == L"storage_cleanup_status")
            {
                return L"topology.storage_probe_failed";
            }
            if (command == L"payload" ||
                command == L"phase_status" ||
                command == L"phase_begin" ||
                command == L"phase_complete")
            {
                return L"topology.app_service_payload_invalid";
            }
            return L"topology.app_service_response_failed";
        }
    }

    [[noreturn]] void Fail(
        std::wstring const& code,
        std::wstring const& message,
        std::wstring const& detail)
    {
        throw Error{ code, message, detail };
    }

    bool IsSafeToken(std::wstring const& value, size_t maximumLength)
    {
        if (value.empty() || value.size() > maximumLength)
        {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](wchar_t ch)
        {
            return
                (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9') ||
                ch == L'.' || ch == L'_' ||
                ch == L'-' || ch == L':';
        });
    }

    bool IsLowerHex(std::wstring const& value, size_t length)
    {
        return value.size() == length &&
            std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return
                    (ch >= L'0' && ch <= L'9') ||
                    (ch >= L'a' && ch <= L'f');
            });
    }

    std::wstring RequiredString(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode)
    {
        if (!object || !object.HasKey(name) ||
            object.GetNamedValue(name).ValueType() != JsonValueType::String)
        {
            Fail(
                errorCode,
                L"a required string field is absent or has the wrong type",
                name);
        }
        return std::wstring(object.GetNamedString(name).c_str());
    }

    bool RequiredBool(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode)
    {
        if (!object || !object.HasKey(name) ||
            object.GetNamedValue(name).ValueType() != JsonValueType::Boolean)
        {
            Fail(
                errorCode,
                L"a required boolean field is absent or has the wrong type",
                name);
        }
        return object.GetNamedBoolean(name);
    }

    uint64_t RequiredUInt64(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode)
    {
        if (!object || !object.HasKey(name) ||
            object.GetNamedValue(name).ValueType() != JsonValueType::Number)
        {
            Fail(
                errorCode,
                L"a required integer field is absent or has the wrong type",
                name);
        }
        auto number = object.GetNamedNumber(name);
        if (!std::isfinite(number) || number < 0.0 ||
            number > 9007199254740991.0 ||
            std::floor(number) != number)
        {
            Fail(
                errorCode,
                L"a required integer is outside the exact JSON range",
                name);
        }
        return static_cast<uint64_t>(number);
    }

    uint64_t OptionalUInt64(
        JsonObject const& object,
        wchar_t const* name,
        uint64_t fallback)
    {
        if (!object || !object.HasKey(name))
        {
            return fallback;
        }
        return RequiredUInt64(
            object,
            name,
            L"topology.app_service_payload_invalid");
    }

    JsonObject RequiredObject(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring const& errorCode)
    {
        if (!object || !object.HasKey(name) ||
            object.GetNamedValue(name).ValueType() != JsonValueType::Object)
        {
            Fail(
                errorCode,
                L"a required object field is absent or has the wrong type",
                name);
        }
        return object.GetNamedObject(name);
    }

    JsonObject OptionalObject(
        JsonObject const& object,
        wchar_t const* name)
    {
        if (!object || !object.HasKey(name) ||
            object.GetNamedValue(name).ValueType() != JsonValueType::Object)
        {
            return JsonObject{ nullptr };
        }
        return object.GetNamedObject(name);
    }

    uint64_t NestedUInt64(
        JsonObject const& object,
        wchar_t const* child,
        wchar_t const* name,
        uint64_t fallback)
    {
        auto nested = OptionalObject(object, child);
        return nested
            ? OptionalUInt64(nested, name, fallback)
            : fallback;
    }

    void PutString(
        JsonObject const& object,
        wchar_t const* name,
        std::wstring_view value)
    {
        object.SetNamedValue(
            name,
            JsonValue::CreateStringValue(hstring(value)));
    }

    void PutNumber(
        JsonObject const& object,
        wchar_t const* name,
        uint64_t value)
    {
        object.SetNamedValue(
            name,
            JsonValue::CreateNumberValue(
                static_cast<double>(value)));
    }

    void PutDouble(
        JsonObject const& object,
        wchar_t const* name,
        double value)
    {
        object.SetNamedValue(
            name,
            JsonValue::CreateNumberValue(value));
    }

    void PutBool(
        JsonObject const& object,
        wchar_t const* name,
        bool value)
    {
        object.SetNamedValue(
            name,
            JsonValue::CreateBooleanValue(value));
    }

    void PutNull(
        JsonObject const& object,
        wchar_t const* name)
    {
        object.SetNamedValue(
            name,
            JsonValue::CreateNullValue());
    }

    JsonArray StableErrorCodes()
    {
        JsonArray values;
        for (auto const* code : {
            L"topology.request_schema_invalid",
            L"topology.memory_request_invalid",
            L"topology.app_service_open_failed",
            L"topology.app_service_timeout",
            L"topology.app_service_response_failed",
            L"topology.app_service_payload_invalid",
            L"topology.memory_allocation_failed",
            L"topology.memory_verification_failed",
            L"topology.storage_probe_failed" })
        {
            values.Append(JsonValue::CreateStringValue(code));
        }
        return values;
    }

    JsonObject ClaimBoundary()
    {
        JsonObject value;
        PutBool(value, L"bounded_diagnostic_probe", true);
        PutBool(value, L"xbox_public_uwp", true);
        PutBool(value, L"restricted_capability_required", false);
        PutBool(value, L"full_trust", false);
        PutBool(value, L"process_spawning", false);
        PutBool(value, L"shell", false);
        PutBool(value, L"runtime_native_codegen", false);
        PutBool(value, L"runtime_shader_compilation", false);
        PutBool(value, L"client_native_payload", false);
        PutBool(value, L"client_shader_payload", false);
        PutBool(value, L"storage_written", true);
        PutBool(value, L"storage_authority", false);
        PutBool(value, L"artifact_publication", false);
        PutBool(value, L"semantic_authority", false);
        PutBool(value, L"independent_memory_budget_proven", false);
        PutBool(value, L"production_authority", false);

        PutBool(value, L"broker_semantic_authority", false);
        PutBool(value, L"ssd_staging_admitted", false);
        PutBool(value, L"predetermined_5_plus_5_gib_claim", false);
        PutBool(value, L"arbitrary_native_or_host_code", false);
        PutBool(value, L"shell_or_process_spawn", false);
        PutBool(value, L"restricted_capability_drift", false);
        return value;
    }

    JsonObject ErrorCodes(std::wstring const& observed)
    {
        JsonObject value;
        if (observed.empty())
        {
            PutNull(value, L"observed");
        }
        else
        {
            PutString(value, L"observed", observed);
        }
        value.SetNamedValue(L"stable", StableErrorCodes());
        return value;
    }

    std::wstring ErrorEnvelope(
        std::wstring const& protocolVersion,
        std::wstring const& correlationId,
        Error const& error)
    {
        JsonObject value;
        PutString(value, L"schema_version", ResultSchema);
        PutBool(value, L"ok", false);
        PutString(value, L"protocol_version", protocolVersion);
        PutString(value, L"command", L"probe_process_topology");
        PutString(value, L"gate_id", GateId);
        PutString(value, L"correlation_id", correlationId);
        PutString(value, L"decision", L"ERROR");
        PutNull(value, L"package");
        PutNull(value, L"foreground");
        PutNull(value, L"broker");
        PutNull(value, L"memory_relation");
        PutNull(value, L"ipc");
        PutNull(value, L"storage");
        auto structuredRefusal = static_cast<bool>(error.refusalEvidence);
        if (structuredRefusal)
        {
            value.SetNamedValue(L"refusal_evidence", error.refusalEvidence);
        }
        else
        {
            PutNull(value, L"refusal_evidence");
        }

        JsonObject lifecycle;
        PutBool(lifecycle, L"foreground_release_completed", structuredRefusal);
        PutBool(lifecycle, L"broker_release_completed", structuredRefusal);
        PutBool(lifecycle, L"client_close_invoked", structuredRefusal);
        PutBool(lifecycle, L"client_close_completed", structuredRefusal);
        PutBool(lifecycle, L"app_service_closed_event_observed", false);
        PutBool(lifecycle, L"app_service_closed", false);
        value.SetNamedValue(L"lifecycle", lifecycle);
        value.SetNamedValue(L"claim_boundary", ClaimBoundary());
        value.SetNamedValue(L"error_codes", ErrorCodes(error.code));

        JsonObject detail;
        PutString(detail, L"code", error.code);
        PutString(detail, L"message", error.message);
        PutString(detail, L"detail", error.detail);
        value.SetNamedValue(L"error", detail);
        return std::wstring(value.Stringify().c_str());
    }

    JsonObject NewBrokerCommand(std::wstring_view command)
    {
        JsonObject value;
        PutString(
            value,
            L"schema_version",
            BrokerRequestSchema);
        PutString(value, L"command", command);
        return value;
    }

    BrokerReply SendBrokerCommand(
        WorkerProcessTopologyTransport& transport,
        JsonObject const& request,
        std::chrono::milliseconds timeout)
    {
        auto command = RequiredString(
            request,
            L"command",
            L"topology.app_service_payload_invalid");
        auto exchange = transport.Exchange(
            request.Stringify(),
            timeout);

        JsonObject envelope{ nullptr };
        try
        {
            envelope = JsonObject::Parse(
                exchange.responseJson);
        }
        catch (...)
        {
            Fail(
                L"topology.app_service_payload_invalid",
                L"the broker response_json is not valid JSON");
        }

        auto schema = RequiredString(
            envelope,
            L"schema_version",
            L"topology.app_service_payload_invalid");
        auto responseCommand = RequiredString(
            envelope,
            L"command",
            L"topology.app_service_payload_invalid");
        auto activationId = RequiredString(
            envelope,
            L"activation_id",
            L"topology.app_service_payload_invalid");
        auto ok = RequiredBool(
            envelope,
            L"ok",
            L"topology.app_service_payload_invalid");
        if (schema != BrokerResponseSchema ||
            responseCommand != command ||
            activationId.empty() ||
            activationId.size() > 128)
        {
            Fail(
                L"topology.app_service_payload_invalid",
                L"the broker response identity is inconsistent");
        }
        if (!ok)
        {
            auto code = RequiredString(
                envelope,
                L"code",
                L"topology.app_service_payload_invalid");
            std::wstring detail = L"broker rejected the command";
            if (envelope.HasKey(L"detail") &&
                envelope.GetNamedValue(L"detail").ValueType() ==
                    JsonValueType::String)
            {
                detail =
                    std::wstring(envelope.GetNamedString(L"detail").c_str());
            }
            Fail(BrokerFailureCode(command), detail, code);
        }

        BrokerReply reply;
        reply.result = RequiredObject(
            envelope,
            L"result",
            L"topology.app_service_payload_invalid");
        reply.activationId = activationId;
        reply.roundTripMilliseconds =
            ElapsedMilliseconds(exchange.sendStarted);
        reply.responseUtf8Bytes =
            exchange.responseUtf8Bytes;
        return reply;
    }

    void TryReleaseBroker(
        WorkerProcessTopologyTransport& transport,
        std::chrono::milliseconds timeout) noexcept
    {
        if (timeout.count() <= 0)
        {
            return;
        }
        try
        {
            SendBrokerCommand(
                transport,
                NewBrokerCommand(L"release"),
                timeout);
        }
        catch (...)
        {
        }
    }

    std::wstring PayloadSha256(uint64_t bytes)
    {
        std::vector<uint8_t> payload(
            static_cast<size_t>(bytes),
            static_cast<uint8_t>('x'));
        auto provider = HashAlgorithmProvider::OpenAlgorithm(
            HashAlgorithmNames::Sha256());
        auto value = std::wstring(
            CryptographicBuffer::EncodeToHexString(
                provider.HashData(
                    CryptographicBuffer::CreateFromByteArray(
                        payload))).c_str());
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    JsonObject HeartbeatProjection(
        uint64_t interval,
        uint64_t expected,
        uint64_t observed,
        uint64_t maximumGap,
        bool independentWhileMemoryRetained)
    {
        JsonObject value;
        PutNumber(value, L"interval_ms", interval);
        PutNumber(value, L"samples_expected", expected);
        PutNumber(value, L"samples_observed", observed);
        PutNumber(value, L"maximum_gap_ms", maximumGap);
        PutBool(
            value,
            L"independent_while_memory_retained",
            independentWhileMemoryRetained);
        return value;
    }

    JsonObject MemoryProjection(
        uint64_t appLimit,
        uint64_t appUsage,
        uint64_t processCommit,
        uint64_t processResident,
        uint64_t virtualReserved,
        uint64_t requestedTouched,
        uint64_t touched,
        std::wstring const& digest,
        uint64_t mutationCount,
        double retentionSeconds,
        uint64_t limitEventCount)
    {
        JsonObject value;
        PutNumber(
            value,
            L"app_memory_usage_limit_bytes",
            appLimit);
        PutNumber(
            value,
            L"app_memory_usage_bytes",
            appUsage);
        PutNumber(
            value,
            L"process_commit_bytes",
            processCommit);
        PutNumber(
            value,
            L"process_resident_bytes",
            processResident);
        PutNumber(
            value,
            L"virtual_reserved_bytes",
            virtualReserved);
        PutNumber(
            value,
            L"requested_touched_bytes",
            requestedTouched);
        PutNumber(value, L"touched_bytes", touched);
        PutNumber(value, L"verified_bytes", touched);
        PutString(value, L"digest_sha256", digest);
        PutNumber(
            value,
            L"periodic_mutation_count",
            mutationCount);
        PutDouble(
            value,
            L"retention_window_seconds",
            retentionSeconds);
        PutNumber(
            value,
            L"limit_change_event_count",
            limitEventCount);
        return value;
    }

    JsonObject ComponentProjection(
        std::wstring const& componentId,
        std::wstring const& role,
        std::wstring const& packageIdentity,
        std::wstring const& processIdentity,
        std::wstring const& activationIdentity,
        std::wstring const& lifecycleState,
        JsonObject const& memory,
        JsonObject const& heartbeat,
        bool cpuSchedulingObserved)
    {
        JsonObject value;
        PutString(value, L"component_id", componentId);
        PutString(value, L"role", role);
        PutString(
            value,
            L"package_identity",
            packageIdentity);
        PutString(
            value,
            L"process_identity",
            processIdentity);
        PutString(
            value,
            L"activation_identity",
            activationIdentity);
        PutString(
            value,
            L"lifecycle_state",
            lifecycleState);
        value.SetNamedValue(L"memory", memory);
        value.SetNamedValue(L"heartbeat", heartbeat);
        PutBool(
            value,
            L"cpu_scheduling_observed",
            cpuSchedulingObserved);
        return value;
    }

    std::wstring SuccessEnvelope(
        std::wstring const& protocolVersion,
        std::wstring const& correlationId,
        JsonObject const& package,
        JsonObject const& foreground,
        JsonObject const& broker,
        JsonObject const& memoryRelation,
        JsonObject const& ipc,
        JsonObject const& lifecycle,
        JsonObject const& storage)
    {
        JsonObject result;
        PutString(result, L"schema_version", ResultSchema);
        PutBool(result, L"ok", true);
        PutString(
            result,
            L"protocol_version",
            protocolVersion);
        PutString(
            result,
            L"command",
            L"probe_process_topology");
        PutString(result, L"gate_id", GateId);
        PutString(
            result,
            L"correlation_id",
            correlationId);
        PutString(
            result,
            L"decision",
            L"DUAL_PROCESS_TOPOLOGY_OBSERVED_MEMORY_RELATION_INDETERMINATE");
        result.SetNamedValue(L"package", package);
        result.SetNamedValue(L"foreground", foreground);
        result.SetNamedValue(L"broker", broker);
        result.SetNamedValue(
            L"memory_relation",
            memoryRelation);
        result.SetNamedValue(L"ipc", ipc);
        result.SetNamedValue(L"lifecycle", lifecycle);
        if (storage)
        {
            result.SetNamedValue(L"storage", storage);
        }
        else
        {
            PutNull(result, L"storage");
        }
        result.SetNamedValue(
            L"claim_boundary",
            ClaimBoundary());
        result.SetNamedValue(
            L"error_codes",
            ErrorCodes(L""));
        return std::wstring(result.Stringify().c_str());
    }
}

namespace XComputeProbe
{
    std::wstring WorkerProcessTopologyCapabilityJson()
    {
        using namespace ProcessTopology;

        JsonObject value;
        PutString(value, L"schema_version", CapabilitySchema);
        PutString(value, L"gate_id", GateId);
        PutString(
            value,
            L"capability_id",
            L"xcp.process.topology.probe.v1");
        PutString(
            value,
            L"implementation_status",
            L"RUNTIME_MODULE_REQUIRES_BUILD_DEPLOY_LIVE_MEASUREMENT");
        PutBool(value, L"packaged_contract", true);
        PutBool(value, L"live_measured", false);
        PutString(
            value,
            L"app_service_name",
            AppServiceName);
        PutString(
            value,
            L"request_schema",
            RequestSchema);
        PutString(
            value,
            L"result_schema",
            ResultSchema);
        PutString(value, L"broker_request_schema", BrokerRequestSchema);
        PutString(value, L"broker_response_schema", BrokerResponseSchema);
        PutString(
            value,
            L"broker_phase_status_schema",
            BrokerPhaseStatusSchema);
        PutString(
            value,
            L"broker_phase_control_schema",
            BrokerPhaseControlSchema);
        PutString(
            value,
            L"topology_phase_runtime_request_schema",
            PhaseRequestSchema);
        PutString(
            value,
            L"historical_topology_phase_runtime_request_schema",
            PhaseRequestSchemaV1);
        PutString(
            value,
            L"lifecycle_journal_schema",
            XComputeShared::XcpProcessTopologyLifecycleJournal::SchemaVersion);
        PutNumber(
            value,
            L"maximum_lifecycle_journal_events",
            XComputeShared::XcpProcessTopologyLifecycleJournal::MaximumEvents);
        PutBool(value, L"persisted_lifecycle_handoff_supported", true);
        PutString(value, L"digest_field", L"digest_sha256");
        PutString(
            value,
            L"heartbeat_semantics",
            L"foreground_thread_verifies_previous_generation_then_mutates;_broker_retention_command_verifies_before_optional_mutate;_broker_autonomous_heartbeat_uses_periodic_thread_pool_timer");
        PutBool(value, L"broker_autonomous_heartbeat", true);
        PutNumber(
            value,
            L"broker_autonomous_heartbeat_interval_ms",
            BrokerAutonomousHeartbeatIntervalMilliseconds);
        PutBool(value, L"broker_phase_status_read_only", true);
        PutBool(value, L"broker_phase_assignment_supported", true);
        PutBool(
            value,
            L"full_t2_phase_instrumentation_complete",
            true);
        PutBool(
            value,
            L"broker_phase_evidence_acceptance_authority",
            false);
        PutBool(
            value,
            L"broker_phase_classification_authority",
            false);
        PutNumber(
            value,
            L"runtime_maximum_touched_bytes_per_component",
            MaximumTouchedBytes);
        PutNumber(
            value,
            L"broker_advertised_maximum_allocation_bytes",
            4294967296ull);
        PutNumber(
            value,
            L"maximum_heartbeat_count",
            MaximumHeartbeatCount);
        PutNumber(
            value,
            L"maximum_heartbeat_window_ms",
            MaximumHeartbeatWindowMilliseconds);
        PutNumber(
            value,
            L"maximum_ipc_payload_bytes",
            MaximumPayloadBytes);
        PutBool(value, L"ipc_latency_distribution_complete", true);
        PutBool(value, L"ipc_bidirectional_throughput", true);
        PutBool(value, L"bidirectional_storage_visibility_probe", true);
        PutNumber(value, L"maximum_storage_probe_bytes", MaximumStorageProbeBytes);
        PutBool(value, L"storage_authority", false);
        PutNumber(value, L"operation_deadline_ms", MaximumOperationDeadlineMilliseconds);
        PutNumber(value, L"maximum_aggregate_touched_round_bytes", MaximumAggregateTouchedRoundBytes);
        PutNumber(value, L"maximum_aggregate_payload_round_bytes", MaximumAggregatePayloadRoundBytes);
        value.SetNamedValue(
            L"stable_error_codes",
            StableErrorCodes());
        value.SetNamedValue(
            L"claim_boundary",
            ClaimBoundary());

        try
        {
            PutString(
                value,
                L"package_family_name",
                Package::Current().Id().FamilyName().c_str());
        }
        catch (...)
        {
            PutString(
                value,
                L"package_family_name",
                L"");
        }
        return std::wstring(value.Stringify().c_str());
    }
}