#include "pch.h"
#include "WorkerXvmSnapshot.h"

#include "WorkerXvmInterpreter.h"
#include "../ProbeResult.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* SnapshotSchemaVersionV2Value = L"xvm-state-snapshot-v2";
        constexpr wchar_t const* SnapshotSchemaVersionV3Value = L"xvm-state-snapshot-v3";
        constexpr wchar_t const* SnapshotSchemaVersionV4Value = L"xvm-state-snapshot-v4";
        constexpr wchar_t const* SnapshotSchemaVersionV5Value = L"xvm-state-snapshot-v5";
        constexpr wchar_t const* StateDigestSchemaVersionV1Value = L"xvm-machine-state-digest-v1";
        constexpr wchar_t const* StateDigestSchemaVersionV2Value = L"xvm-machine-state-digest-v2";
        constexpr wchar_t const* StateDigestSchemaVersionV3Value = L"xvm-machine-state-digest-v3";
        constexpr wchar_t const* StateDigestSchemaVersionV4Value = L"xvm-machine-state-digest-v4";
        constexpr wchar_t const* TrapStateSchemaVersionValue = L"xvm-trap-state-v1";
        constexpr wchar_t const* SnapshotProducerValue = L"xcompute-worker";

        bool IsLowerHex(std::wstring const& value, size_t length);

        wchar_t const* SnapshotSchemaVersion(WorkerXvmSnapshotContext const& context)
        {
            return context.packagedCapsuleCpuState
                ? SnapshotSchemaVersionV5Value
                : (context.tieredCpuState
                    ? SnapshotSchemaVersionV4Value
                    : (context.structuredControlPhaseB
                        ? SnapshotSchemaVersionV3Value
                        : SnapshotSchemaVersionV2Value));
        }

        wchar_t const* StateDigestSchemaVersion(WorkerXvmSnapshotContext const& context)
        {
            return context.packagedCapsuleCpuState
                ? StateDigestSchemaVersionV4Value
                : (context.tieredCpuState
                    ? StateDigestSchemaVersionV3Value
                    : (context.structuredControlPhaseB
                        ? StateDigestSchemaVersionV2Value
                        : StateDigestSchemaVersionV1Value));
        }

        std::wstring CpuContextFieldsJson(WorkerXvmSnapshotContext const& context)
        {
            if (!context.tieredCpuState && !context.packagedCapsuleCpuState)
            {
                return L"";
            }
            auto fields = std::wstring(L",\"execution_plan_sha256\":") +
                JsonString(context.executionPlanSha256) +
                L",\"source_backend\":" + JsonString(context.sourceBackend);
            if (context.packagedCapsuleCpuState)
            {
                fields += std::wstring(L",\"profile_contract_sha256\":") +
                    JsonString(context.profileContractSha256) +
                    L",\"module_sha256\":" + JsonString(context.moduleSha256);
            }
            return fields;
        }

        void ValidateSnapshotContext(WorkerXvmSnapshotContext const& context)
        {
            if (context.tieredCpuState && context.packagedCapsuleCpuState)
            {
                throw WorkerXvmError(
                    "xvm.snapshot_context_invalid",
                    "XVM snapshot context cannot select two CPU source backends");
            }
            if (context.packagedCapsuleCpuState)
            {
                if (!IsLowerHex(context.executionPlanSha256, 64) ||
                    context.sourceBackend != L"cpu_packaged_capsule_hot_kernel" ||
                    !IsLowerHex(context.profileContractSha256, 64) ||
                    !IsLowerHex(context.moduleSha256, 64))
                {
                    throw WorkerXvmError(
                        "xvm.snapshot_context_invalid",
                        "packaged capsule snapshot context requires plan, profile, module and source-backend bindings");
                }
            }
            else if (context.tieredCpuState)
            {
                if (!IsLowerHex(context.executionPlanSha256, 64) ||
                    context.sourceBackend != L"cpu_plan_codelet" ||
                    !context.profileContractSha256.empty() ||
                    !context.moduleSha256.empty())
                {
                    throw WorkerXvmError(
                        "xvm.snapshot_context_invalid",
                        "tiered CPU snapshot context requires the admitted plan digest and source backend");
                }
            }
            else if (!context.executionPlanSha256.empty() || !context.sourceBackend.empty() ||
                !context.profileContractSha256.empty() || !context.moduleSha256.empty())
            {
                throw WorkerXvmError(
                    "xvm.snapshot_context_invalid",
                    "reference snapshots may not carry specialized CPU context fields");
            }
        }

        std::wstring TrapStateJson(WorkerXvmTrapState const& trap)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(TrapStateSchemaVersionValue) +
                L",\"status\":" + JsonString(trap.occurrenceCount == 0 ? L"none" : L"recovered") +
                L",\"code\":" + std::to_wstring(trap.code) +
                L",\"trap_pc\":" + std::to_wstring(trap.trapPc) +
                L",\"recovery_pc\":" + std::to_wstring(trap.recoveryPc) +
                L",\"occurrence_count\":" + JsonString(std::to_wstring(trap.occurrenceCount)) + L"}";
        }

        std::wstring JoinJson(std::vector<std::wstring> const& values)
        {
            std::wstring joined = L"[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    joined += L",";
                }
                joined += values[index];
            }
            joined += L"]";
            return joined;
        }

        bool IsSafeId(std::wstring const& value)
        {
            return !value.empty() && value.size() <= 64 &&
                std::all_of(value.begin(), value.end(), [](wchar_t ch)
                {
                    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
                });
        }

        bool IsLowerHex(std::wstring const& value, size_t length)
        {
            return value.size() == length && std::all_of(value.begin(), value.end(), [](wchar_t ch)
            {
                return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
            });
        }

        bool ConstantTimeEqual(std::wstring const& left, std::wstring const& right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            wchar_t difference = 0;
            for (size_t index = 0; index < left.size(); ++index)
            {
                difference = static_cast<wchar_t>(difference | (left[index] ^ right[index]));
            }
            return difference == 0;
        }

        uint32_t ReadU32(JsonObject const& object, wchar_t const* name)
        {
            auto value = object.GetNamedValue(name);
            if (value.ValueType() != JsonValueType::Number)
            {
                throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot unsigned integer field has the wrong JSON type");
            }
            auto number = value.GetNumber();
            if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number)
            {
                throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot unsigned integer field is outside uint32");
            }
            return static_cast<uint32_t>(number);
        }

        uint64_t ReadU64DecimalString(JsonObject const& object, wchar_t const* name)
        {
            auto value = std::wstring(object.GetNamedString(name).c_str());
            if (value.empty() || (value.size() > 1 && value.front() == L'0') ||
                !std::all_of(value.begin(), value.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
            {
                throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot uint64 field is not canonical decimal");
            }
            try
            {
                size_t consumed = 0;
                auto parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size())
                {
                    throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot uint64 field has a trailing value");
                }
                return parsed;
            }
            catch (WorkerXvmError const&)
            {
                throw;
            }
            catch (...)
            {
                throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot uint64 field is outside range");
            }
        }

        std::vector<uint8_t> ReadCanonicalHex(JsonObject const& object, wchar_t const* name, size_t expectedBytes)
        {
            auto value = std::wstring(object.GetNamedString(name).c_str());
            if (value.size() != expectedBytes * 2 || !IsLowerHex(value, value.size()))
            {
                throw WorkerXvmError("xvm.snapshot_hex_invalid", "XVM snapshot byte field is not canonical lowercase hex of the admitted size");
            }
            std::vector<uint8_t> bytes(expectedBytes, 0);
            auto nibble = [](wchar_t ch)
            {
                return static_cast<uint8_t>(ch <= L'9' ? ch - L'0' : ch - L'a' + 10);
            };
            for (size_t index = 0; index < expectedBytes; ++index)
            {
                bytes[index] = static_cast<uint8_t>((nibble(value[index * 2]) << 4) | nibble(value[index * 2 + 1]));
            }
            return bytes;
        }

        void RequireObjectSize(JsonObject const& object, uint32_t expected)
        {
            if (object.Size() != expected)
            {
                throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot contains missing or unadmitted fields");
            }
        }

        std::wstring MachineStateFieldsJson(WorkerXvmMachineState const& state, bool includeTrapState)
        {
            std::vector<std::wstring> registers;
            registers.reserve(state.registers.size());
            for (auto value : state.registers)
            {
                registers.push_back(std::to_wstring(value));
            }
            std::vector<std::wstring> loops;
            loops.reserve(state.loopStack.size());
            for (auto const& frame : state.loopStack)
            {
                loops.push_back(std::wstring(L"{\"begin_pc\":") + std::to_wstring(frame.beginPc) +
                    L",\"end_pc\":" + std::to_wstring(frame.endPc) +
                    L",\"remaining\":" + std::to_wstring(frame.remaining) + L"}");
            }
            std::vector<std::wstring> calls;
            calls.reserve(state.callStack.size());
            for (auto const& frame : state.callStack)
            {
                calls.push_back(std::wstring(L"{\"return_pc\":") + std::to_wstring(frame.returnPc) +
                    L",\"loop_depth\":" + std::to_wstring(frame.loopDepth) + L"}");
            }
            auto fields = std::wstring(L",\"pc\":") + std::to_wstring(state.pc) +
                L",\"fuel_consumed\":" + JsonString(std::to_wstring(state.fuelConsumed)) +
                L",\"halted\":" + (state.halted ? L"true" : L"false") +
                L",\"control_token\":" + JsonString(state.controlToken) +
                L",\"registers\":" + JoinJson(registers) +
                L",\"memory_hex\":" + JsonString(WorkerXvmBytesHex(state.memory)) +
                L",\"output_hex\":" + JsonString(WorkerXvmBytesHex(state.output)) +
                L",\"loop_stack\":" + JoinJson(loops) +
                L",\"call_stack\":" + JoinJson(calls);
            if (includeTrapState)
            {
                fields += L",\"trap_state\":" + TrapStateJson(state.trap);
            }
            return fields;
        }

        std::wstring SnapshotUnsignedJson(
            WorkerXvmSnapshotContext const& context,
            WorkerXvmSnapshotProvenance const& provenance,
            WorkerXvmMachineState const& state,
            WorkerXvmSnapshotAuthority const& authority)
        {
            return std::wstring(L"{\"schema_version\":") + JsonString(SnapshotSchemaVersion(context)) +
                L",\"isa_version\":" + JsonString(context.isaVersion) +
                L",\"program_id\":" + JsonString(context.programId) +
                L",\"program_sha256\":" + JsonString(context.programSha256) +
                L",\"bound_input_sha256\":" + JsonString(context.boundInputSha256) +
                CpuContextFieldsJson(context) +
                L",\"execution_id\":" + JsonString(provenance.executionId) +
                L",\"checkpoint_sequence\":" + JsonString(std::to_wstring(provenance.checkpointSequence)) +
                L",\"previous_state_sha256\":" + JsonString(provenance.previousStateSha256) +
                L",\"producer\":" + JsonString(SnapshotProducerValue) +
                L",\"seal_schema_version\":" + JsonString(WorkerXvmSnapshotSealSchemaVersion()) +
                L",\"seal_key_id\":" + JsonString(authority.keyId) +
                MachineStateFieldsJson(state, context.structuredControlPhaseB) + L"}";
        }

        WorkerXvmMachineState ParseMachineState(
            JsonObject const& root,
            WorkerGraphNodeResourceLimits const& limits,
            bool includeTrapState)
        {
            WorkerXvmMachineState state;
            state.pc = ReadU32(root, L"pc");
            state.fuelConsumed = ReadU64DecimalString(root, L"fuel_consumed");
            state.halted = root.GetNamedBoolean(L"halted");
            state.controlToken = std::wstring(root.GetNamedString(L"control_token").c_str());
            auto registers = root.GetNamedArray(L"registers");
            if (registers.Size() != WorkerXvmRegisterCountValue)
            {
                throw WorkerXvmError("xvm.snapshot_shape_invalid", "XVM snapshot register file has the wrong size");
            }
            state.registers.reserve(registers.Size());
            for (uint32_t index = 0; index < registers.Size(); ++index)
            {
                JsonObject holder;
                holder.Insert(L"value", registers.GetAt(index));
                state.registers.push_back(ReadU32(holder, L"value"));
            }
            state.memory = ReadCanonicalHex(root, L"memory_hex", static_cast<size_t>(limits.memoryBytes));
            state.output = ReadCanonicalHex(root, L"output_hex", static_cast<size_t>(limits.outputBytes));

            auto loops = root.GetNamedArray(L"loop_stack");
            for (uint32_t index = 0; index < loops.Size(); ++index)
            {
                auto object = loops.GetObjectAt(index);
                RequireObjectSize(object, 3);
                state.loopStack.push_back({ ReadU32(object, L"begin_pc"), ReadU32(object, L"end_pc"), ReadU32(object, L"remaining") });
            }
            auto calls = root.GetNamedArray(L"call_stack");
            for (uint32_t index = 0; index < calls.Size(); ++index)
            {
                auto object = calls.GetObjectAt(index);
                RequireObjectSize(object, 2);
                state.callStack.push_back({ ReadU32(object, L"return_pc"), ReadU32(object, L"loop_depth") });
            }
            if (includeTrapState)
            {
                auto trap = root.GetNamedObject(L"trap_state");
                RequireObjectSize(trap, 6);
                if (std::wstring(trap.GetNamedString(L"schema_version").c_str()) != TrapStateSchemaVersionValue)
                {
                    throw WorkerXvmError("xvm.snapshot_trap_state_invalid", "XVM trap state schema is not admitted");
                }
                auto status = std::wstring(trap.GetNamedString(L"status").c_str());
                state.trap.code = ReadU32(trap, L"code");
                state.trap.trapPc = ReadU32(trap, L"trap_pc");
                state.trap.recoveryPc = ReadU32(trap, L"recovery_pc");
                state.trap.occurrenceCount = ReadU64DecimalString(trap, L"occurrence_count");
                if ((state.trap.occurrenceCount == 0 && status != L"none") ||
                    (state.trap.occurrenceCount != 0 && status != L"recovered"))
                {
                    throw WorkerXvmError("xvm.snapshot_trap_state_invalid", "XVM trap status does not match its occurrence count");
                }
            }
            return state;
        }
    }

    wchar_t const* WorkerXvmStateSnapshotSchemaVersion()
    {
        return SnapshotSchemaVersionV2Value;
    }

    wchar_t const* WorkerXvmLatestStateSnapshotSchemaVersion()
    {
        return SnapshotSchemaVersionV5Value;
    }

    wchar_t const* WorkerXvmStateSnapshotSchemaVersionForProgram(WorkerXvmProgram const& program)
    {
        return std::find(program.capabilities.begin(), program.capabilities.end(), L"structured_control_v2_phase_b") != program.capabilities.end()
            ? SnapshotSchemaVersionV3Value
            : SnapshotSchemaVersionV2Value;
    }

    wchar_t const* WorkerXvmStateSnapshotSchemaVersionForExecution(
        WorkerXvmProgram const& program,
        bool tieredCpuState,
        bool packagedCapsuleCpuState)
    {
        return packagedCapsuleCpuState
            ? SnapshotSchemaVersionV5Value
            : (tieredCpuState
                ? SnapshotSchemaVersionV4Value
                : WorkerXvmStateSnapshotSchemaVersionForProgram(program));
    }

    wchar_t const* WorkerXvmStateDigestSchemaVersion()
    {
        return StateDigestSchemaVersionV1Value;
    }

    std::wstring WorkerXvmMachineStateCanonicalJson(
        WorkerXvmSnapshotContext const& context,
        WorkerXvmMachineState const& state)
    {
        return std::wstring(L"{\"schema_version\":") + JsonString(StateDigestSchemaVersion(context)) +
            L",\"isa_version\":" + JsonString(context.isaVersion) +
            L",\"program_id\":" + JsonString(context.programId) +
            L",\"program_sha256\":" + JsonString(context.programSha256) +
            L",\"bound_input_sha256\":" + JsonString(context.boundInputSha256) +
            CpuContextFieldsJson(context) +
            MachineStateFieldsJson(state, context.structuredControlPhaseB) + L"}";
    }

    std::wstring WorkerXvmStateSnapshotJson(
        WorkerXvmSnapshotContext const& context,
        WorkerXvmSnapshotProvenance const& provenance,
        WorkerXvmMachineState const& state,
        WorkerXvmSnapshotAuthority const& authority)
    {
        if (!authority.Ready())
        {
            throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority is unavailable");
        }
        ValidateSnapshotContext(context);
        if (!IsSafeId(provenance.executionId) || provenance.checkpointSequence == 0 ||
            !IsLowerHex(provenance.previousStateSha256, 64))
        {
            throw WorkerXvmError("xvm.snapshot_provenance_invalid", "XVM snapshot provenance is incomplete or non-canonical");
        }
        auto unsignedJson = SnapshotUnsignedJson(context, provenance, state, authority);
        auto seal = authority.sealCanonical(unsignedJson);
        if (!IsLowerHex(seal, 64))
        {
            throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority returned a non-canonical seal");
        }
        unsignedJson.pop_back();
        return unsignedJson + L",\"worker_seal_sha256\":" + JsonString(seal) + L"}";
    }

    WorkerXvmRestoredSnapshot WorkerRestoreXvmStateSnapshot(
        std::wstring const& snapshotJson,
        WorkerXvmSnapshotContext const& expectedContext,
        WorkerXvmSnapshotResumeAuthorization const* authorization,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerXvmSnapshotAuthority const& authority)
    {
        try
        {
            if (!authority.Ready())
            {
                throw WorkerXvmError("xvm.snapshot_authority_invalid", "worker snapshot authority is unavailable");
            }
            ValidateSnapshotContext(expectedContext);
            auto root = JsonObject::Parse(hstring(snapshotJson));
            RequireObjectSize(root, expectedContext.packagedCapsuleCpuState
                ? 26
                : (expectedContext.tieredCpuState
                    ? 24
                    : (expectedContext.structuredControlPhaseB ? 22 : 21)));
            if (std::wstring(root.GetNamedString(L"schema_version").c_str()) != SnapshotSchemaVersion(expectedContext) ||
                std::wstring(root.GetNamedString(L"isa_version").c_str()) != expectedContext.isaVersion ||
                std::wstring(root.GetNamedString(L"program_id").c_str()) != expectedContext.programId ||
                std::wstring(root.GetNamedString(L"program_sha256").c_str()) != expectedContext.programSha256 ||
                std::wstring(root.GetNamedString(L"bound_input_sha256").c_str()) != expectedContext.boundInputSha256 ||
                ((expectedContext.tieredCpuState || expectedContext.packagedCapsuleCpuState) &&
                    (std::wstring(root.GetNamedString(L"execution_plan_sha256").c_str()) != expectedContext.executionPlanSha256 ||
                     std::wstring(root.GetNamedString(L"source_backend").c_str()) != expectedContext.sourceBackend)) ||
                (expectedContext.packagedCapsuleCpuState &&
                    (std::wstring(root.GetNamedString(L"profile_contract_sha256").c_str()) != expectedContext.profileContractSha256 ||
                     std::wstring(root.GetNamedString(L"module_sha256").c_str()) != expectedContext.moduleSha256)))
            {
                throw WorkerXvmError("xvm.snapshot_context_mismatch", "XVM snapshot is not bound to the admitted program, input, execution plan and source backend");
            }

            WorkerXvmSnapshotProvenance provenance;
            provenance.executionId = std::wstring(root.GetNamedString(L"execution_id").c_str());
            provenance.checkpointSequence = ReadU64DecimalString(root, L"checkpoint_sequence");
            provenance.previousStateSha256 = std::wstring(root.GetNamedString(L"previous_state_sha256").c_str());
            auto producer = std::wstring(root.GetNamedString(L"producer").c_str());
            auto sealSchema = std::wstring(root.GetNamedString(L"seal_schema_version").c_str());
            auto sealKeyId = std::wstring(root.GetNamedString(L"seal_key_id").c_str());
            auto workerSeal = std::wstring(root.GetNamedString(L"worker_seal_sha256").c_str());
            if (!IsSafeId(provenance.executionId) || provenance.checkpointSequence == 0 ||
                !IsLowerHex(provenance.previousStateSha256, 64) || producer != SnapshotProducerValue ||
                sealSchema != WorkerXvmSnapshotSealSchemaVersion() || !IsLowerHex(sealKeyId, 64) ||
                !IsLowerHex(workerSeal, 64))
            {
                throw WorkerXvmError("xvm.snapshot_provenance_invalid", "XVM snapshot provenance fields are invalid");
            }
            if (sealKeyId != authority.keyId)
            {
                throw WorkerXvmError("xvm.snapshot_seal_invalid", "XVM snapshot was not sealed by the active worker authority");
            }

            auto state = ParseMachineState(root, limits, expectedContext.structuredControlPhaseB);
            WorkerValidateXvmMachineState(program, limits, state);
            auto expectedSeal = authority.sealCanonical(SnapshotUnsignedJson(expectedContext, provenance, state, authority));
            if (!ConstantTimeEqual(workerSeal, expectedSeal))
            {
                throw WorkerXvmError("xvm.snapshot_seal_invalid", "XVM snapshot worker seal verification failed");
            }
            if (authorization &&
                (provenance.executionId != authorization->expectedExecutionId ||
                 provenance.checkpointSequence != authorization->expectedCheckpointSequence ||
                 provenance.previousStateSha256 != authorization->expectedPreviousStateSha256))
            {
                throw WorkerXvmError("xvm.resume_snapshot_unauthorized", "XVM resume policy did not authorize the sealed snapshot provenance");
            }

            WorkerXvmRestoredSnapshot restored;
            restored.state = std::move(state);
            restored.provenance = std::move(provenance);
            restored.workerSealSha256 = std::move(workerSeal);
            return restored;
        }
        catch (WorkerXvmError const&)
        {
            throw;
        }
        catch (...)
        {
            throw WorkerXvmError("xvm.snapshot_json_invalid", "XVM snapshot is not valid canonical JSON state");
        }
    }
}