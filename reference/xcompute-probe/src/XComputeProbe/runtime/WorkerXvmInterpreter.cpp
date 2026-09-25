#include "pch.h"
#include "WorkerXvmInterpreter.h"

#include "WorkerXvmIsa.h"

namespace XComputeProbe
{
    namespace
    {
        uint32_t InstructionWord(WorkerXvmProgram const& program, uint32_t pc, uint32_t word)
        {
            return program.words[(static_cast<size_t>(pc) * WorkerXvmInstructionWords()) + word];
        }

        uint32_t RotateLeft32(uint32_t value, uint32_t count)
        {
            count &= 31u;
            return count == 0 ? value : static_cast<uint32_t>((value << count) | (value >> (32u - count)));
        }

        uint32_t ReadU32(std::vector<uint8_t> const& bytes, uint32_t offset)
        {
            return static_cast<uint32_t>(bytes[offset]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }

        void WriteU32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
        {
            bytes[offset] = static_cast<uint8_t>(value & 0xffu);
            bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
            bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
            bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
        }

        bool TypedMemoryEnabled(WorkerXvmProgram const& program)
        {
            return std::find(program.capabilities.begin(), program.capabilities.end(), L"typed_memory_v1") != program.capabilities.end();
        }
        bool StructuredControlEnabled(WorkerXvmProgram const& program)
        {
            return std::find(program.capabilities.begin(), program.capabilities.end(), L"structured_control_v2") != program.capabilities.end();
        }

        bool StructuredControlPhaseBEnabled(WorkerXvmProgram const& program)
        {
            return std::find(program.capabilities.begin(), program.capabilities.end(), L"structured_control_v2_phase_b") != program.capabilities.end();
        }

        void RequireTypedViewAccess(
            WorkerXvmProgram const& program,
            std::vector<WorkerXvmTypedView> const& views,
            uint64_t offsetBytes,
            WorkerXvmViewAccess access)
        {
            if (!TypedMemoryEnabled(program))
            {
                return;
            }
            auto admitted = std::any_of(views.begin(), views.end(), [&](WorkerXvmTypedView const& view)
            {
                auto accessAllowed = view.access == WorkerXvmViewAccess::ReadWrite || view.access == access;
                return accessAllowed &&
                    offsetBytes >= view.offsetBytes &&
                    offsetBytes + 4 <= view.offsetBytes + view.lengthBytes;
            });
            if (!admitted)
            {
                throw WorkerXvmError("xvm.typed_view_runtime_violation", "runtime typed-memory access escaped its verified u32 view");
            }
        }
    }

    WorkerXvmMachineState WorkerCreateXvmMachineState(
        WorkerGraphNodeResourceLimits const& limits)
    {
        WorkerXvmMachineState state;
        state.registers.resize(WorkerXvmRegisterCountValue, 0);
        state.memory.resize(static_cast<size_t>(limits.memoryBytes), 0);
        state.output.resize(static_cast<size_t>(limits.outputBytes), 0);
        return state;
    }

    void WorkerValidateXvmMachineState(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerXvmMachineState const& state)
    {
        auto instructionCount = static_cast<uint32_t>(program.words.size() / WorkerXvmInstructionWords());
        if (state.registers.size() != WorkerXvmRegisterCountValue ||
            state.memory.size() != limits.memoryBytes ||
            state.output.size() != limits.outputBytes)
        {
            throw WorkerXvmError("xvm.snapshot_shape_invalid", "XVM snapshot register, memory, or output shape does not match admission");
        }
        if (state.pc > instructionCount || state.fuelConsumed > limits.fuel ||
            (state.controlToken != L"pass" && state.controlToken != L"fail"))
        {
            throw WorkerXvmError("xvm.snapshot_state_invalid", "XVM snapshot scalar state is outside the admitted machine contract");
        }
        if (state.loopStack.size() > WorkerXvmLoopDepthLimit() || state.callStack.size() > program.maxCallDepth)
        {
            throw WorkerXvmError("xvm.snapshot_stack_invalid", "XVM snapshot stack depth exceeds admission");
        }
        auto phaseB = StructuredControlPhaseBEnabled(program);
        if (!phaseB && (state.trap.code != 0 || state.trap.trapPc != 0 || state.trap.recoveryPc != 0 || state.trap.occurrenceCount != 0))
        {
            throw WorkerXvmError("xvm.snapshot_trap_state_invalid", "non-Phase-B XVM state may not contain trap state");
        }
        if (phaseB)
        {
            if (state.trap.occurrenceCount == 0)
            {
                if (state.trap.code != 0 || state.trap.trapPc != 0 || state.trap.recoveryPc != 0)
                {
                    throw WorkerXvmError("xvm.snapshot_trap_state_invalid", "empty XVM trap state must use canonical zero fields");
                }
            }
            else if (state.trap.code == 0 || state.trap.code > WorkerXvmTrapCodeLimit() ||
                state.trap.trapPc >= instructionCount || state.trap.recoveryPc >= instructionCount ||
                InstructionWord(program, state.trap.trapPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::TrapIfZero) ||
                InstructionWord(program, state.trap.trapPc, 2) != state.trap.code ||
                InstructionWord(program, state.trap.trapPc, 3) != state.trap.recoveryPc)
            {
                throw WorkerXvmError("xvm.snapshot_trap_state_invalid", "recovered XVM trap state does not match the admitted program");
            }
        }
        auto regionForPc = [&](uint32_t pc) -> size_t
        {
            if (program.functions.empty())
            {
                return 0;
            }
            if (pc < program.functions.front().entryPc)
            {
                return 0;
            }
            for (size_t index = 0; index < program.functions.size(); ++index)
            {
                auto const& function = program.functions[index];
                if (pc >= function.entryPc && pc <= function.endPc)
                {
                    return index + 1;
                }
            }
            throw WorkerXvmError("xvm.snapshot_pc_invalid", "XVM snapshot pc is not owned by an admitted code region");
        };
        auto functionRegionForEntry = [&](uint32_t entryPc) -> size_t
        {
            for (size_t index = 0; index < program.functions.size(); ++index)
            {
                if (program.functions[index].entryPc == entryPc)
                {
                    return index + 1;
                }
            }
            throw WorkerXvmError("xvm.snapshot_call_frame_invalid", "XVM snapshot call target is not a declared function entry");
        };

        for (auto const& frame : state.loopStack)
        {
            if (frame.beginPc >= frame.endPc || frame.endPc >= instructionCount || frame.remaining == 0 ||
                frame.remaining > InstructionWord(program, frame.beginPc, 1) ||
                InstructionWord(program, frame.beginPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::LoopBegin) ||
                InstructionWord(program, frame.beginPc, 2) != frame.endPc ||
                InstructionWord(program, frame.endPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::LoopEnd) ||
                InstructionWord(program, frame.endPc, 1) != frame.beginPc ||
                regionForPc(frame.beginPc) != regionForPc(frame.endPc))
            {
                throw WorkerXvmError("xvm.snapshot_loop_frame_invalid", "XVM snapshot loop frame does not match the admitted program region");
            }
        }

        size_t activeRegion = 0;
        uint32_t assignedLoopDepth = 0;
        auto validateLoopSegment = [&](uint32_t endDepth, size_t expectedRegion, uint32_t executionPc)
        {
            if (endDepth < assignedLoopDepth || endDepth > state.loopStack.size())
            {
                throw WorkerXvmError("xvm.snapshot_call_frame_invalid", "XVM snapshot call frame loop depth is not monotonic");
            }
            uint32_t previousBegin = 0;
            uint32_t previousEnd = UINT32_MAX;
            for (uint32_t index = assignedLoopDepth; index < endDepth; ++index)
            {
                auto const& loop = state.loopStack[index];
                if (regionForPc(loop.beginPc) != expectedRegion ||
                    !(loop.beginPc < executionPc && executionPc <= loop.endPc) ||
                    (index != assignedLoopDepth && !(previousBegin < loop.beginPc && loop.endPc <= previousEnd)))
                {
                    throw WorkerXvmError("xvm.snapshot_loop_frame_invalid", "XVM snapshot loop stack is not an active nested region chain");
                }
                previousBegin = loop.beginPc;
                previousEnd = loop.endPc;
            }
            assignedLoopDepth = endDepth;
        };

        for (auto const& frame : state.callStack)
        {
            if (program.functions.empty() || frame.returnPc == 0 || frame.returnPc > instructionCount ||
                frame.loopDepth > state.loopStack.size())
            {
                throw WorkerXvmError("xvm.snapshot_call_frame_invalid", "XVM snapshot call frame is outside the admitted program");
            }
            auto callPc = frame.returnPc - 1;
            if (regionForPc(callPc) != activeRegion ||
                InstructionWord(program, callPc, 0) != static_cast<uint32_t>(WorkerXvmOpcode::Call))
            {
                throw WorkerXvmError("xvm.snapshot_call_frame_invalid", "XVM snapshot return address is not an admitted call site in the active caller region");
            }
            validateLoopSegment(frame.loopDepth, activeRegion, callPc);
            activeRegion = functionRegionForEntry(InstructionWord(program, callPc, 1));
        }

        if (!state.halted)
        {
            if (regionForPc(state.pc) != activeRegion)
            {
                throw WorkerXvmError("xvm.snapshot_pc_invalid", "XVM snapshot pc does not match the active call chain region");
            }
            validateLoopSegment(static_cast<uint32_t>(state.loopStack.size()), activeRegion, state.pc);
        }
        else if (assignedLoopDepth != state.loopStack.size())
        {
            throw WorkerXvmError("xvm.snapshot_loop_frame_invalid", "halted XVM snapshot has unassigned loop frames");
        }
        if (state.halted)
        {
            if (state.pc != instructionCount || !state.loopStack.empty() || !state.callStack.empty())
            {
                throw WorkerXvmError("xvm.snapshot_halt_invalid", "halted XVM snapshot must be terminal with balanced stacks");
            }
        }
        else if (state.pc >= instructionCount)
        {
            throw WorkerXvmError("xvm.snapshot_pc_invalid", "non-terminal XVM snapshot pc must address an admitted instruction");
        }
    }

    WorkerXvmAdvanceStatus WorkerAdvanceXvmCpuReference(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState& state,
        uint64_t checkpointFuel,
        std::function<bool()> const& cancelRequested)
    {
        WorkerValidateXvmMachineState(program, limits, state);
        auto instructionCount = static_cast<uint32_t>(program.words.size() / WorkerXvmInstructionWords());
        while (!state.halted && state.pc < instructionCount)
        {
            if (cancelRequested && cancelRequested())
            {
                return WorkerXvmAdvanceStatus::Canceled;
            }
            if (checkpointFuel != 0 && state.fuelConsumed >= checkpointFuel)
            {
                return WorkerXvmAdvanceStatus::Checkpoint;
            }
            if (state.fuelConsumed >= limits.fuel)
            {
                throw WorkerXvmError("xvm.fuel_exhausted", "XVM execution exhausted admitted node fuel");
            }
            ++state.fuelConsumed;

            auto opcode = static_cast<WorkerXvmOpcode>(InstructionWord(program, state.pc, 0));
            auto a = InstructionWord(program, state.pc, 1);
            auto b = InstructionWord(program, state.pc, 2);
            auto c = InstructionWord(program, state.pc, 3);
            switch (opcode)
            {
            case WorkerXvmOpcode::Halt:
                if (!state.callStack.empty())
                {
                    throw WorkerXvmError("xvm.halt_runtime_invalid", "halt encountered with active call frames");
                }
                state.halted = true;
                state.pc = instructionCount;
                break;
            case WorkerXvmOpcode::MoveImmediate:
                state.registers[a] = b;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoadInputU32:
                RequireTypedViewAccess(program, program.inputViews, static_cast<uint64_t>(b) * 4, WorkerXvmViewAccess::Read);
                state.registers[a] = inputs[b];
                ++state.pc;
                break;
            case WorkerXvmOpcode::AddU32:
                state.registers[a] = state.registers[b] + state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::XorU32:
                state.registers[a] = state.registers[b] ^ state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::MultiplyU32:
                state.registers[a] = state.registers[b] * state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::RotateLeftU32:
                state.registers[a] = RotateLeft32(state.registers[b], c);
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoadMemoryU32:
                RequireTypedViewAccess(program, program.memoryViews, b, WorkerXvmViewAccess::Read);
                state.registers[a] = ReadU32(state.memory, b);
                ++state.pc;
                break;
            case WorkerXvmOpcode::StoreMemoryU32:
                RequireTypedViewAccess(program, program.memoryViews, b, WorkerXvmViewAccess::Write);
                WriteU32(state.memory, b, state.registers[a]);
                ++state.pc;
                break;
            case WorkerXvmOpcode::EqualU32:
                state.registers[a] = state.registers[b] == state.registers[c] ? 1u : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LessThanU32:
                if (!StructuredControlPhaseBEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_runtime_violation", "runtime ordered comparison was not admitted");
                }
                state.registers[a] = state.registers[b] < state.registers[c] ? 1u : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::LessThanS32:
                if (!StructuredControlPhaseBEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_runtime_violation", "runtime signed comparison was not admitted");
                }
                state.registers[a] = (state.registers[b] ^ 0x80000000u) < (state.registers[c] ^ 0x80000000u) ? 1u : 0u;
                ++state.pc;
                break;
            case WorkerXvmOpcode::SelectU32:
                if (!StructuredControlPhaseBEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_runtime_violation", "runtime select was not admitted");
                }
                state.registers[a] = state.registers[a] != 0 ? state.registers[b] : state.registers[c];
                ++state.pc;
                break;
            case WorkerXvmOpcode::BranchIfZero:
                state.pc = state.registers[a] == 0 ? b : state.pc + 1;
                break;
            case WorkerXvmOpcode::LoopBegin:
                if (state.loopStack.size() >= WorkerXvmLoopDepthLimit())
                {
                    throw WorkerXvmError("xvm.loop_depth_exceeded", "runtime loop stack exceeds the admitted depth");
                }
                state.loopStack.push_back({ state.pc, b, a });
                ++state.pc;
                break;
            case WorkerXvmOpcode::LoopEnd:
                if (state.loopStack.empty() || state.loopStack.back().beginPc != a || state.loopStack.back().endPc != state.pc)
                {
                    throw WorkerXvmError("xvm.loop_runtime_invalid", "runtime loop frame does not match admitted program");
                }
                if (state.loopStack.back().remaining > 1)
                {
                    --state.loopStack.back().remaining;
                    state.pc = state.loopStack.back().beginPc + 1;
                }
                else
                {
                    state.loopStack.pop_back();
                    ++state.pc;
                }
                break;
            case WorkerXvmOpcode::BreakIfZero:
            case WorkerXvmOpcode::ContinueIfZero:
                if (!StructuredControlPhaseBEnabled(program) || state.loopStack.empty() ||
                    state.loopStack.back().beginPc != b || state.loopStack.back().endPc != c)
                {
                    throw WorkerXvmError("xvm.structured_control_loop_runtime_violation", "runtime loop control escaped its verified innermost loop");
                }
                if (state.registers[a] == 0)
                {
                    if (opcode == WorkerXvmOpcode::BreakIfZero)
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
                RequireTypedViewAccess(program, program.outputViews, b, WorkerXvmViewAccess::Write);
                WriteU32(state.output, b, state.registers[a]);
                ++state.pc;
                break;
            case WorkerXvmOpcode::SetControl:
                state.controlToken = state.registers[a] == 0 ? L"fail" : L"pass";
                ++state.pc;
                break;
            case WorkerXvmOpcode::Call:
                if (state.callStack.size() >= program.maxCallDepth)
                {
                    throw WorkerXvmError("xvm.call_depth_exceeded", "runtime call stack exceeds the verified max_call_depth");
                }
                state.callStack.push_back({ state.pc + 1, static_cast<uint32_t>(state.loopStack.size()) });
                state.pc = a;
                break;
            case WorkerXvmOpcode::Return:
                if (state.callStack.empty() || state.loopStack.size() != state.callStack.back().loopDepth)
                {
                    throw WorkerXvmError("xvm.return_runtime_invalid", "return does not match an admitted call frame");
                }
                state.pc = state.callStack.back().returnPc;
                state.callStack.pop_back();
                break;
            case WorkerXvmOpcode::IfZero:
                if (!StructuredControlEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_runtime_violation", "runtime structured control was not admitted");
                }
                state.pc = state.registers[a] == 0 ? (b < c ? b + 1 : c + 1) : state.pc + 1;
                break;
            case WorkerXvmOpcode::Else:
                if (!StructuredControlEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_runtime_violation", "runtime else was not admitted");
                }
                state.pc = a + 1;
                break;
            case WorkerXvmOpcode::EndIf:
                if (!StructuredControlEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_runtime_violation", "runtime end_if was not admitted");
                }
                ++state.pc;
                break;
            case WorkerXvmOpcode::TrapIfZero:
                if (!StructuredControlPhaseBEnabled(program))
                {
                    throw WorkerXvmError("xvm.structured_control_phase_b_runtime_violation", "runtime deterministic trap was not admitted");
                }
                if (state.registers[a] == 0)
                {
                    if (state.trap.occurrenceCount == UINT64_MAX)
                    {
                        throw WorkerXvmError("xvm.structured_control_trap_runtime_violation", "runtime trap occurrence count overflowed");
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
                throw WorkerXvmError("xvm.opcode_runtime_invalid", "runtime encountered a non-admitted opcode");
            }
        }
        WorkerValidateXvmMachineState(program, limits, state);
        if (!state.halted)
        {
            throw WorkerXvmError("xvm.execution_incomplete", "program did not halt with balanced loop and call stacks");
        }
        return WorkerXvmAdvanceStatus::Halted;
    }

    WorkerXvmRunResult WorkerXvmMachineResult(WorkerXvmMachineState const& state)
    {
        if (!state.halted)
        {
            throw WorkerXvmError("xvm.execution_incomplete", "cannot produce a result from a non-terminal XVM machine state");
        }
        WorkerXvmRunResult result;
        result.output = state.output;
        result.controlToken = state.controlToken;
        result.fuelConsumed = state.fuelConsumed;
        result.trap = state.trap;
        return result;
    }

    WorkerXvmRunResult WorkerRunXvmCpuReference(
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs)
    {
        auto state = WorkerCreateXvmMachineState(limits);
        (void)WorkerAdvanceXvmCpuReference(program, limits, inputs, state, 0);
        return WorkerXvmMachineResult(state);
    }

    std::wstring WorkerXvmBytesHex(std::vector<uint8_t> const& bytes)
    {
        std::wostringstream out;
        out << std::hex << std::setfill(L'0');
        for (auto byte : bytes)
        {
            out << std::setw(2) << static_cast<uint32_t>(byte);
        }
        return out.str();
    }
}
