#include "pch.h"
#include "WorkerXvmCpuExecutionPlan.h"

#include "WorkerGpuXvmProfileContract.h"
#include "../ProbeResult.h"

using namespace winrt;

namespace XComputeProbe
{
    namespace
    {
        enum class PrepareStatus
        {
            Ready,
            Checkpoint,
            Canceled,
        };

        uint32_t InstructionWord(
            WorkerXvmProgram const& program,
            uint32_t pc,
            uint32_t word)
        {
            return program.words[
                (static_cast<size_t>(pc) * WorkerXvmInstructionWordsValue) + word];
        }

        uint32_t RotateLeft32(uint32_t value, uint32_t count)
        {
            count &= 31u;
            return count == 0
                ? value
                : static_cast<uint32_t>(
                    (value << count) | (value >> (32u - count)));
        }

        uint32_t ReadU32(std::vector<uint8_t> const& bytes, uint32_t offset)
        {
            return static_cast<uint32_t>(bytes[offset]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }

        void WriteU32(
            std::vector<uint8_t>& bytes,
            uint32_t offset,
            uint32_t value)
        {
            bytes[offset] = static_cast<uint8_t>(value & 0xffu);
            bytes[offset + 1] =
                static_cast<uint8_t>((value >> 8) & 0xffu);
            bytes[offset + 2] =
                static_cast<uint8_t>((value >> 16) & 0xffu);
            bytes[offset + 3] =
                static_cast<uint8_t>((value >> 24) & 0xffu);
        }

        bool HasCapability(
            WorkerXvmProgram const& program,
            wchar_t const* capability)
        {
            return std::find(
                program.capabilities.begin(),
                program.capabilities.end(),
                capability) != program.capabilities.end();
        }

        void RequireTypedViewAccess(
            WorkerXvmProgram const& program,
            std::vector<WorkerXvmTypedView> const& views,
            uint64_t offsetBytes,
            WorkerXvmViewAccess access)
        {
            if (!HasCapability(program, L"typed_memory_v1"))
            {
                return;
            }
            auto admitted = std::any_of(
                views.begin(),
                views.end(),
                [&](WorkerXvmTypedView const& view)
                {
                    auto accessAllowed =
                        view.access == WorkerXvmViewAccess::ReadWrite ||
                        view.access == access;
                    return accessAllowed &&
                        offsetBytes >= view.offsetBytes &&
                        offsetBytes + 4 <=
                            view.offsetBytes + view.lengthBytes;
                });
            if (!admitted)
            {
                throw WorkerXvmError(
                    "xvm.typed_view_runtime_violation",
                    "runtime typed-memory access escaped its verified u32 view");
            }
        }

        PrepareStatus PrepareInstruction(
            WorkerGraphNodeResourceLimits const& limits,
            WorkerXvmMachineState& state,
            uint64_t checkpointFuel,
            WorkerXvmCpuPlanTelemetry& telemetry,
            std::function<bool()> const& cancelRequested)
        {
            if (cancelRequested && cancelRequested())
            {
                return PrepareStatus::Canceled;
            }
            if (checkpointFuel != 0 &&
                state.fuelConsumed >= checkpointFuel)
            {
                return PrepareStatus::Checkpoint;
            }
            if (state.fuelConsumed >= limits.fuel)
            {
                throw WorkerXvmError(
                    "xvm.fuel_exhausted",
                    "XVM execution exhausted admitted node fuel");
            }
            ++state.fuelConsumed;
            ++telemetry.sourceInstructionsExecuted;
            return PrepareStatus::Ready;
        }

        WorkerXvmAdvanceStatus AdvanceStatus(PrepareStatus status)
        {
            return status == PrepareStatus::Canceled
                ? WorkerXvmAdvanceStatus::Canceled
                : WorkerXvmAdvanceStatus::Checkpoint;
        }

        void ExecuteInstruction(
            WorkerXvmProgram const& program,
            WorkerXvmCpuPlanInstruction const& instruction,
            uint32_t instructionCount,
            WorkerXvmMachineState& state,
            std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs)
        {
            auto a = instruction.a;
            auto b = instruction.b;
            auto c = instruction.c;
            switch (instruction.opcode)
            {
            case WorkerXvmOpcode::Halt:
                if (!state.callStack.empty())
                {
                    throw WorkerXvmError(
                        "xvm.halt_runtime_invalid",
                        "halt encountered with active call frames");
                }
                state.halted = true;
                state.pc = instructionCount;
                break;
            case WorkerXvmOpcode::MoveImmediate:
                state.registers[a] = b;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoadInputU32:
                RequireTypedViewAccess(
                    program,
                    program.inputViews,
                    static_cast<uint64_t>(b) * 4,
                    WorkerXvmViewAccess::Read);
                state.registers[a] = inputs[b];
                ++state.pc;
                break;
            case WorkerXvmOpcode::AddU32:
                state.registers[a] =
                    state.registers[b] + state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::XorU32:
                state.registers[a] =
                    state.registers[b] ^ state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::MultiplyU32:
                state.registers[a] =
                    state.registers[b] * state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::RotateLeftU32:
                state.registers[a] =
                    RotateLeft32(state.registers[b], c);
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoadMemoryU32:
                RequireTypedViewAccess(
                    program,
                    program.memoryViews,
                    b,
                    WorkerXvmViewAccess::Read);
                state.registers[a] = ReadU32(state.memory, b);
                ++state.pc;
                break;
            case WorkerXvmOpcode::StoreMemoryU32:
                RequireTypedViewAccess(
                    program,
                    program.memoryViews,
                    b,
                    WorkerXvmViewAccess::Write);
                WriteU32(state.memory, b, state.registers[a]);
                ++state.pc;
                break;
            case WorkerXvmOpcode::EqualU32:
                state.registers[a] =
                    state.registers[b] == state.registers[c] ? 1u : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LessThanU32:
                if (!HasCapability(
                        program,
                        L"structured_control_v2_phase_b"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_phase_b_runtime_violation",
                        "runtime ordered comparison was not admitted");
                }
                state.registers[a] =
                    state.registers[b] < state.registers[c] ? 1u : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LessThanS32:
                if (!HasCapability(
                        program,
                        L"structured_control_v2_phase_b"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_phase_b_runtime_violation",
                        "runtime signed comparison was not admitted");
                }
                state.registers[a] =
                    (state.registers[b] ^ 0x80000000u) <
                    (state.registers[c] ^ 0x80000000u)
                    ? 1u
                    : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::SelectU32:
                if (!HasCapability(
                        program,
                        L"structured_control_v2_phase_b"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_phase_b_runtime_violation",
                        "runtime select was not admitted");
                }
                state.registers[a] = state.registers[a] != 0
                    ? state.registers[b]
                    : state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::BranchIfZero:
                state.pc =
                    state.registers[a] == 0 ? b : state.pc + 1;
                break;
            case WorkerXvmOpcode::LoopBegin:
                if (state.loopStack.size() >=
                    WorkerXvmLoopDepthLimit())
                {
                    throw WorkerXvmError(
                        "xvm.loop_depth_exceeded",
                        "runtime loop stack exceeds the admitted depth");
                }
                state.loopStack.push_back({ state.pc, b, a });
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoopEnd:
                if (state.loopStack.empty() ||
                    state.loopStack.back().beginPc != a ||
                    state.loopStack.back().endPc != state.pc)
                {
                    throw WorkerXvmError(
                        "xvm.loop_runtime_invalid",
                        "runtime loop frame does not match admitted program");
                }
                if (state.loopStack.back().remaining > 1)
                {
                    --state.loopStack.back().remaining;
                    state.pc =
                        state.loopStack.back().beginPc + 1;
                }
                else
                {
                    state.loopStack.pop_back();
                    ++state.pc;
                }
                break;
            case WorkerXvmOpcode::BreakIfZero:
            case WorkerXvmOpcode::ContinueIfZero:
                if (!HasCapability(
                        program,
                        L"structured_control_v2_phase_b") ||
                    state.loopStack.empty() ||
                    state.loopStack.back().beginPc != b ||
                    state.loopStack.back().endPc != c)
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_loop_runtime_violation",
                        "runtime loop control escaped its verified innermost loop");
                }
                if (state.registers[a] == 0)
                {
                    if (instruction.opcode ==
                        WorkerXvmOpcode::BreakIfZero)
                    {
                        state.loopStack.pop_back();
                        state.pc = c + 1;
                    }
                    else
                    {
                        state.pc = c;
                    }
                }
                else
                {
                    ++state.pc;
                }
                break;
            case WorkerXvmOpcode::OutputU32:
                RequireTypedViewAccess(
                    program,
                    program.outputViews,
                    b,
                    WorkerXvmViewAccess::Write);
                WriteU32(state.output, b, state.registers[a]);
                ++state.pc;
                break;
            case WorkerXvmOpcode::SetControl:
                state.controlToken =
                    state.registers[a] == 0 ? L"fail" : L"pass";
                ++state.pc;
                break;
            case WorkerXvmOpcode::Call:
                if (state.callStack.size() >=
                    program.maxCallDepth)
                {
                    throw WorkerXvmError(
                        "xvm.call_depth_exceeded",
                        "runtime call stack exceeds the verified max_call_depth");
                }
                state.callStack.push_back({
                    state.pc + 1,
                    static_cast<uint32_t>(state.loopStack.size()),
                });
                state.pc = a;
                break;
            case WorkerXvmOpcode::Return:
                if (state.callStack.empty() ||
                    state.loopStack.size() !=
                        state.callStack.back().loopDepth)
                {
                    throw WorkerXvmError(
                        "xvm.return_runtime_invalid",
                        "return does not match an admitted call frame");
                }
                state.pc = state.callStack.back().returnPc;
                state.callStack.pop_back();
                break;
            case WorkerXvmOpcode::IfZero:
                if (!HasCapability(
                        program,
                        L"structured_control_v2"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_runtime_violation",
                        "runtime structured control was not admitted");
                }
                state.pc = state.registers[a] == 0
                    ? (b < c ? b + 1 : c + 1)
                    : state.pc + 1;
                break;
            case WorkerXvmOpcode::Else:
                if (!HasCapability(
                        program,
                        L"structured_control_v2"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_runtime_violation",
                        "runtime else was not admitted");
                }
                state.pc = a + 1;
                break;
            case WorkerXvmOpcode::EndIf:
                if (!HasCapability(
                        program,
                        L"structured_control_v2"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_runtime_violation",
                        "runtime end_if was not admitted");
                }
                ++state.pc;
                break;
            case WorkerXvmOpcode::TrapIfZero:
                if (!HasCapability(
                        program,
                        L"structured_control_v2_phase_b"))
                {
                    throw WorkerXvmError(
                        "xvm.structured_control_phase_b_runtime_violation",
                        "runtime deterministic trap was not admitted");
                }
                if (state.registers[a] == 0)
                {
                    if (state.trap.occurrenceCount == UINT64_MAX)
                    {
                        throw WorkerXvmError(
                            "xvm.structured_control_trap_runtime_violation",
                            "runtime trap occurrence count overflowed");
                    }
                    state.trap.code = b;
                    state.trap.trapPc = state.pc;
                    state.trap.recoveryPc = c;
                    ++state.trap.occurrenceCount;
                    state.pc = c;
                }
                else
                {
                    ++state.pc;
                }
                break;
            default:
                throw WorkerXvmError(
                    "xvm.opcode_runtime_invalid",
                    "runtime encountered a non-admitted opcode");
            }
        }

        void MarkTarget(
            std::vector<bool>& targets,
            uint32_t target)
        {
            if (target < targets.size())
            {
                targets[target] = true;
            }
        }

        std::vector<bool> BuildEntryTargets(
            WorkerXvmProgram const& program,
            uint32_t instructionCount)
        {
            std::vector<bool> targets(instructionCount, false);
            if (!targets.empty())
            {
                targets[0] = true;
            }
            for (auto const& function : program.functions)
            {
                MarkTarget(targets, function.entryPc);
            }
            for (uint32_t pc = 0; pc < instructionCount; ++pc)
            {
                auto opcode = static_cast<WorkerXvmOpcode>(
                    InstructionWord(program, pc, 0));
                auto a = InstructionWord(program, pc, 1);
                auto b = InstructionWord(program, pc, 2);
                auto c = InstructionWord(program, pc, 3);
                switch (opcode)
                {
                case WorkerXvmOpcode::BranchIfZero:
                    MarkTarget(targets, b);
                    break;
                case WorkerXvmOpcode::LoopBegin:
                    MarkTarget(targets, pc + 1);
                    MarkTarget(targets, b + 1);
                    break;
                case WorkerXvmOpcode::LoopEnd:
                    MarkTarget(targets, a + 1);
                    MarkTarget(targets, pc + 1);
                    break;
                case WorkerXvmOpcode::Call:
                    MarkTarget(targets, a);
                    MarkTarget(targets, pc + 1);
                    break;
                case WorkerXvmOpcode::IfZero:
                    MarkTarget(targets, pc + 1);
                    MarkTarget(targets, b + 1);
                    MarkTarget(targets, c + 1);
                    break;
                case WorkerXvmOpcode::Else:
                    MarkTarget(targets, a + 1);
                    break;
                case WorkerXvmOpcode::BreakIfZero:
                    MarkTarget(targets, pc + 1);
                    MarkTarget(targets, c + 1);
                    break;
                case WorkerXvmOpcode::ContinueIfZero:
                    MarkTarget(targets, pc + 1);
                    MarkTarget(targets, c);
                    break;
                case WorkerXvmOpcode::TrapIfZero:
                    MarkTarget(targets, pc + 1);
                    MarkTarget(targets, c);
                    break;
                default:
                    break;
                }
            }
            return targets;
        }

        bool CanBuildIntegerMix4(
            WorkerXvmCpuExecutionPlan const& plan,
            std::vector<bool> const& targets,
            uint32_t pc)
        {
            if (pc + 3 >= plan.instructions.size())
            {
                return false;
            }
            for (uint32_t offset = 1; offset < 4; ++offset)
            {
                if (targets[pc + offset])
                {
                    return false;
                }
            }
            return
                plan.instructions[pc].opcode ==
                    WorkerXvmOpcode::AddU32 &&
                plan.instructions[pc + 1].opcode ==
                    WorkerXvmOpcode::XorU32 &&
                plan.instructions[pc + 2].opcode ==
                    WorkerXvmOpcode::MultiplyU32 &&
                plan.instructions[pc + 3].opcode ==
                    WorkerXvmOpcode::RotateLeftU32;
        }

        wchar_t const* CodeletName(
            WorkerXvmCpuCodeletKind kind)
        {
            return kind == WorkerXvmCpuCodeletKind::IntegerMix4
                ? L"integer_mix4_v1"
                : L"instruction_v1";
        }

        std::wstring PlanBindingJson(
            WorkerXvmCpuExecutionPlan const& plan)
        {
            std::wostringstream out;
            out << L"{\"schema_version\":"
                << JsonString(plan.schemaVersion)
                << L",\"codelet_catalog_id\":"
                << JsonString(plan.codeletCatalogId)
                << L",\"program_id\":"
                << JsonString(plan.programId)
                << L",\"program_sha256\":"
                << JsonString(plan.programSha256)
                << L",\"instruction_count\":"
                << plan.instructionCount
                << L",\"static_worst_case_fuel\":"
                << plan.staticWorstCaseFuel
                << L",\"codelet_record_count\":"
                << plan.codeletRecordCount
                << L",\"fused_source_instruction_count\":"
                << plan.fusedSourceInstructionCount
                << L",\"maximum_codelet_span\":"
                << plan.maximumCodeletSpan
                << L",\"estimated_resident_bytes\":"
                << plan.estimatedResidentBytes
                << L",\"instructions\":[";
            for (size_t index = 0;
                 index < plan.instructions.size();
                 ++index)
            {
                if (index != 0)
                {
                    out << L",";
                }
                auto const& instruction = plan.instructions[index];
                out << L"{\"pc\":" << index
                    << L",\"opcode\":"
                    << static_cast<uint32_t>(instruction.opcode)
                    << L",\"a\":" << instruction.a
                    << L",\"b\":" << instruction.b
                    << L",\"c\":" << instruction.c
                    << L",\"codelet\":"
                    << JsonString(CodeletName(
                        instruction.codeletKind))
                    << L",\"source_span\":"
                    << instruction.sourceSpan << L"}";
            }
            out << L"]}";
            return out.str();
        }

        PrepareStatus ExecuteIntegerMix4(
            WorkerXvmCpuExecutionPlan const& plan,
            WorkerGraphNodeResourceLimits const& limits,
            WorkerXvmMachineState& state,
            uint64_t checkpointFuel,
            WorkerXvmCpuPlanTelemetry& telemetry,
            std::function<bool()> const& cancelRequested)
        {
            auto startPc = state.pc;
            constexpr WorkerXvmOpcode expected[] = {
                WorkerXvmOpcode::AddU32,
                WorkerXvmOpcode::XorU32,
                WorkerXvmOpcode::MultiplyU32,
                WorkerXvmOpcode::RotateLeftU32,
            };
            for (uint32_t offset = 0; offset < 4; ++offset)
            {
                auto prepared = PrepareInstruction(
                    limits,
                    state,
                    checkpointFuel,
                    telemetry,
                    cancelRequested);
                if (prepared != PrepareStatus::Ready)
                {
                    return prepared;
                }
                if (state.pc != startPc + offset ||
                    state.pc >= plan.instructions.size())
                {
                    throw WorkerXvmError(
                        "xvm.cpu_plan_runtime_invalid",
                        "CPU codelet source pc diverged from its immutable plan");
                }
                auto const& instruction =
                    plan.instructions[state.pc];
                if (instruction.opcode != expected[offset])
                {
                    throw WorkerXvmError(
                        "xvm.cpu_plan_runtime_invalid",
                        "CPU codelet opcode sequence differs from its immutable plan");
                }
                if (offset == 0)
                {
                    state.registers[instruction.a] =
                        state.registers[instruction.b] +
                        state.registers[instruction.c];
                }
                else if (offset == 1)
                {
                    state.registers[instruction.a] =
                        state.registers[instruction.b] ^
                        state.registers[instruction.c];
                }
                else if (offset == 2)
                {
                    state.registers[instruction.a] =
                        state.registers[instruction.b] *
                        state.registers[instruction.c];
                }
                else
                {
                    state.registers[instruction.a] =
                        RotateLeft32(
                            state.registers[instruction.b],
                            instruction.c);
                }
                ++state.pc;
            }
            return PrepareStatus::Ready;
        }

        bool LoopFramesEqual(
            std::vector<WorkerXvmLoopFrame> const& expected,
            std::vector<WorkerXvmLoopFrame> const& actual)
        {
            if (expected.size() != actual.size())
            {
                return false;
            }
            for (size_t index = 0; index < expected.size(); ++index)
            {
                if (expected[index].beginPc != actual[index].beginPc ||
                    expected[index].endPc != actual[index].endPc ||
                    expected[index].remaining != actual[index].remaining)
                {
                    return false;
                }
            }
            return true;
        }

        bool CallFramesEqual(
            std::vector<WorkerXvmCallFrame> const& expected,
            std::vector<WorkerXvmCallFrame> const& actual)
        {
            if (expected.size() != actual.size())
            {
                return false;
            }
            for (size_t index = 0; index < expected.size(); ++index)
            {
                if (expected[index].returnPc != actual[index].returnPc ||
                    expected[index].loopDepth != actual[index].loopDepth)
                {
                    return false;
                }
            }
            return true;
        }
    }

    WorkerXvmCpuExecutionPlan WorkerBuildXvmCpuExecutionPlan(
        WorkerXvmProgram const& program,
        std::wstring const& programSha256,
        uint64_t staticWorstCaseFuel)
    {
        auto validDigest =
            programSha256.size() == 64 &&
            std::all_of(
                programSha256.begin(),
                programSha256.end(),
                [](wchar_t ch)
                {
                    return
                        (ch >= L'0' && ch <= L'9') ||
                        (ch >= L'a' && ch <= L'f');
                });
        auto instructionCount =
            program.words.size() / WorkerXvmInstructionWordsValue;
        if (!validDigest ||
            instructionCount == 0 ||
            program.words.size() %
                WorkerXvmInstructionWordsValue != 0 ||
            staticWorstCaseFuel == 0)
        {
            throw WorkerXvmError(
                "xvm.cpu_plan_binding_invalid",
                "CPU execution plan requires a verified program, digest and static fuel proof");
        }

        WorkerXvmCpuExecutionPlan plan;
        plan.admitted = true;
        plan.schemaVersion =
            WorkerXvmCpuExecutionPlanSchemaVersion;
        plan.codeletCatalogId =
            WorkerXvmCpuCodeletCatalogId;
        plan.programId = program.programId;
        plan.programSha256 = programSha256;
        plan.instructionCount = instructionCount;
        plan.staticWorstCaseFuel = staticWorstCaseFuel;
        plan.instructions.reserve(instructionCount);
        for (uint32_t pc = 0;
             pc < instructionCount;
             ++pc)
        {
            plan.instructions.push_back({
                static_cast<WorkerXvmOpcode>(
                    InstructionWord(program, pc, 0)),
                InstructionWord(program, pc, 1),
                InstructionWord(program, pc, 2),
                InstructionWord(program, pc, 3),
                WorkerXvmCpuCodeletKind::Instruction,
                1,
            });
        }

        auto targets = BuildEntryTargets(
            program,
            static_cast<uint32_t>(instructionCount));
        for (uint32_t pc = 0;
             pc < instructionCount;)
        {
            if (CanBuildIntegerMix4(plan, targets, pc))
            {
                plan.instructions[pc].codeletKind =
                    WorkerXvmCpuCodeletKind::IntegerMix4;
                plan.instructions[pc].sourceSpan = 4;
                ++plan.codeletRecordCount;
                plan.fusedSourceInstructionCount += 4;
                plan.maximumCodeletSpan = 4;
                pc += 4;
            }
            else
            {
                ++pc;
            }
        }
        plan.estimatedResidentBytes =
            static_cast<uint64_t>(plan.instructions.size()) *
            sizeof(WorkerXvmCpuPlanInstruction);
        auto binding = PlanBindingJson(plan);
        auto utf8 = to_string(hstring(binding));
        plan.planSha256 = WorkerGpuXvmHashBytes(
            std::vector<uint8_t>(utf8.begin(), utf8.end()));
        return plan;
    }

    std::wstring WorkerXvmCpuExecutionPlanJson(
        WorkerXvmCpuExecutionPlan const& plan)
    {
        if (!plan.admitted)
        {
            return L"null";
        }
        auto binding = PlanBindingJson(plan);
        binding.pop_back();
        return binding +
            L",\"plan_sha256\":" +
            JsonString(plan.planSha256) +
            L"}";
    }

    bool WorkerXvmCpuExecutionPlansEqual(
        WorkerXvmCpuExecutionPlan const& expected,
        WorkerXvmCpuExecutionPlan const& actual)
    {
        return expected.admitted == actual.admitted &&
            (!expected.admitted ||
                (expected.schemaVersion == actual.schemaVersion &&
                 expected.codeletCatalogId ==
                    actual.codeletCatalogId &&
                 expected.programId == actual.programId &&
                 expected.programSha256 ==
                    actual.programSha256 &&
                 expected.planSha256 == actual.planSha256 &&
                 expected.instructionCount ==
                    actual.instructionCount &&
                 expected.staticWorstCaseFuel ==
                    actual.staticWorstCaseFuel &&
                 expected.codeletRecordCount ==
                    actual.codeletRecordCount &&
                 expected.fusedSourceInstructionCount ==
                    actual.fusedSourceInstructionCount &&
                 expected.estimatedResidentBytes ==
                    actual.estimatedResidentBytes &&
                 expected.maximumCodeletSpan ==
                    actual.maximumCodeletSpan));
    }

    WorkerXvmAdvanceStatus WorkerAdvanceXvmCpuPlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState& state,
        uint64_t checkpointFuel,
        WorkerXvmCpuPlanTelemetry& telemetry,
        std::function<bool()> const& cancelRequested)
    {
        if (!plan.admitted ||
            plan.instructions.size() != plan.instructionCount ||
            plan.instructionCount !=
                program.words.size() /
                    WorkerXvmInstructionWordsValue)
        {
            throw WorkerXvmError(
                "xvm.cpu_plan_runtime_invalid",
                "CPU execution plan is incomplete or not bound to the admitted program");
        }

        WorkerValidateXvmMachineState(program, limits, state);
        auto instructionCount =
            static_cast<uint32_t>(plan.instructionCount);
        while (!state.halted && state.pc < instructionCount)
        {
            auto const& instruction =
                plan.instructions[state.pc];
            ++telemetry.dispatchCount;
            if (instruction.codeletKind ==
                WorkerXvmCpuCodeletKind::IntegerMix4)
            {
                ++telemetry.codeletDispatchCount;
                auto status = ExecuteIntegerMix4(
                    plan,
                    limits,
                    state,
                    checkpointFuel,
                    telemetry,
                    cancelRequested);
                if (status != PrepareStatus::Ready)
                {
                    return AdvanceStatus(status);
                }
                continue;
            }

            ++telemetry.instructionDispatchCount;
            auto prepared = PrepareInstruction(
                limits,
                state,
                checkpointFuel,
                telemetry,
                cancelRequested);
            if (prepared != PrepareStatus::Ready)
            {
                return AdvanceStatus(prepared);
            }
            ExecuteInstruction(
                program,
                instruction,
                instructionCount,
                state,
                inputs);
        }

        WorkerValidateXvmMachineState(program, limits, state);
        if (!state.halted)
        {
            throw WorkerXvmError(
                "xvm.execution_incomplete",
                "program did not halt with balanced loop and call stacks");
        }
        return WorkerXvmAdvanceStatus::Halted;
    }

    WorkerXvmCpuPlanRun WorkerRunXvmCpuPlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested)
    {
        WorkerXvmCpuPlanRun run;
        auto state = WorkerCreateXvmMachineState(limits);
        auto status = WorkerAdvanceXvmCpuPlan(
            program,
            plan,
            limits,
            inputs,
            state,
            0,
            run.telemetry,
            cancelRequested);
        if (status == WorkerXvmAdvanceStatus::Canceled)
        {
            throw WorkerXvmError(
                "job.canceled",
                "XVM CPU plan execution canceled at a valid instruction boundary");
        }
        run.result = WorkerXvmMachineResult(state);
        return run;
    }

    bool WorkerXvmMachineStatesEqual(
        WorkerXvmMachineState const& expected,
        WorkerXvmMachineState const& actual)
    {
        return expected.registers == actual.registers &&
            expected.memory == actual.memory &&
            expected.output == actual.output &&
            LoopFramesEqual(expected.loopStack, actual.loopStack) &&
            CallFramesEqual(expected.callStack, actual.callStack) &&
            expected.controlToken == actual.controlToken &&
            expected.fuelConsumed == actual.fuelConsumed &&
            expected.pc == actual.pc &&
            expected.trap.code == actual.trap.code &&
            expected.trap.trapPc == actual.trap.trapPc &&
            expected.trap.recoveryPc == actual.trap.recoveryPc &&
            expected.trap.occurrenceCount ==
                actual.trap.occurrenceCount &&
            expected.halted == actual.halted;
    }

    bool WorkerXvmRunResultsEqual(
        WorkerXvmRunResult const& expected,
        WorkerXvmRunResult const& actual)
    {
        return expected.output == actual.output &&
            expected.controlToken == actual.controlToken &&
            expected.fuelConsumed == actual.fuelConsumed &&
            expected.trap.code == actual.trap.code &&
            expected.trap.trapPc == actual.trap.trapPc &&
            expected.trap.recoveryPc == actual.trap.recoveryPc &&
            expected.trap.occurrenceCount ==
                actual.trap.occurrenceCount;
    }
}
