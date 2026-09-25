StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
StructuredBuffer<uint> Microtrace : register(t3);
RWStructuredBuffer<uint> State : register(u0);

#define XVM_REGISTER_COUNT 16
#define XVM_MEMORY_WORDS 16
#define XVM_OUTPUT_WORDS 4
#define XVM_MAX_INSTRUCTIONS 32
#define XVM_MAX_FUEL 64
#define XVM_MAX_EPOCH_INSTRUCTIONS 8
#define XVM_MAX_EPOCHS 4
#define XVM_TRACE_WORDS_PER_RECORD 16
#define XVM_TRACE_RECORD_SINGLE 0
#define XVM_TRACE_RECORD_PAIR 1
#define XVM_TRACE_RECORD_CONSTANT_FOLD_PAIR 2
#define XVM_TRACE_FLAG_CONSTANT_PROPAGATED 1
#define XVM_OUTPUT_WORD_OFFSET 8
#define XVM_PC_WORD_OFFSET 12
#define XVM_EPOCH_WORD_OFFSET 13
#define XVM_STATUS_WORD_OFFSET 14
#define XVM_EXECUTED_RECORD_WORD_OFFSET 15
#define XVM_REGISTER_WORD_OFFSET 16
#define XVM_MEMORY_WORD_OFFSET 32
#define XVM_STATUS_READY 0
#define XVM_STATUS_RUNNING 1
#define XVM_STATUS_YIELDED 2
#define XVM_STATUS_HALTED 3
#define XVM_STATUS_TRAPPED 4

uint RotateLeft32(uint value, uint shift)
{
    return shift == 0 ? value : ((value << shift) | (value >> (32 - shift)));
}

void Trap(uint code)
{
    State[4] = code;
    State[XVM_STATUS_WORD_OFFSET] = XVM_STATUS_TRAPPED;
}

bool InstructionMatches(uint traceBase, uint instructionOffset, uint sourcePc)
{
    uint programBase = sourcePc * 4;
    uint recordBase = traceBase + instructionOffset;
    return Microtrace[recordBase] == Program[programBase] &&
        Microtrace[recordBase + 1] == Program[programBase + 1] &&
        Microtrace[recordBase + 2] == Program[programBase + 2] &&
        Microtrace[recordBase + 3] == Program[programBase + 3];
}

void ExecuteInstruction(uint opcode, uint a, uint b, uint c, uint pc, uint instructionCount, out uint succeeded)
{
    succeeded = 0;
    if (opcode == 0)
    {
        if (a != 0 || b != 0 || c != 0 || pc + 1 != instructionCount)
        {
            Trap(10);
            return;
        }
        State[0] = 1;
        State[XVM_STATUS_WORD_OFFSET] = XVM_STATUS_HALTED;
        succeeded = 1;
        return;
    }
    if (opcode == 1 && a < XVM_REGISTER_COUNT)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = b;
    }
    else if (opcode == 2 && a < XVM_REGISTER_COUNT && b < 11)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = Inputs[b];
    }
    else if (opcode == 3 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = State[XVM_REGISTER_WORD_OFFSET + b] + State[XVM_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 4 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = State[XVM_REGISTER_WORD_OFFSET + b] ^ State[XVM_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 5 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = State[XVM_REGISTER_WORD_OFFSET + b] * State[XVM_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 6 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < 32)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = RotateLeft32(State[XVM_REGISTER_WORD_OFFSET + b], c);
    }
    else if (opcode == 7 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_MEMORY_WORDS * 4)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = State[XVM_MEMORY_WORD_OFFSET + (b / 4)];
    }
    else if (opcode == 8 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_MEMORY_WORDS * 4)
    {
        State[XVM_MEMORY_WORD_OFFSET + (b / 4)] = State[XVM_REGISTER_WORD_OFFSET + a];
    }
    else if (opcode == 9 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
    {
        State[XVM_REGISTER_WORD_OFFSET + a] = State[XVM_REGISTER_WORD_OFFSET + b] == State[XVM_REGISTER_WORD_OFFSET + c] ? 1 : 0;
    }
    else if (opcode == 13 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_OUTPUT_WORDS * 4)
    {
        State[XVM_OUTPUT_WORD_OFFSET + (b / 4)] = State[XVM_REGISTER_WORD_OFFSET + a];
    }
    else if (opcode == 14 && a < XVM_REGISTER_COUNT)
    {
        State[1] = State[XVM_REGISTER_WORD_OFFSET + a] == 0 ? 0 : 1;
    }
    else
    {
        Trap(10);
        return;
    }
    State[XVM_PC_WORD_OFFSET] = pc + 1;
    succeeded = 1;
}

void ExecuteFoldedSecond(uint traceBase, uint pc, out uint succeeded)
{
    succeeded = 0;
    uint destination = Microtrace[traceBase + 9];
    uint sourceB = Microtrace[traceBase + 10];
    uint sourceC = Microtrace[traceBase + 11];
    uint opcode = Microtrace[traceBase + 8];
    uint foldedValue = Microtrace[traceBase + 12];
    uint selector = Microtrace[traceBase + 13];
    if (destination >= XVM_REGISTER_COUNT)
    {
        Trap(10);
        return;
    }
    if (selector == 3)
    {
        if (opcode != 6 || sourceB >= XVM_REGISTER_COUNT || sourceC >= 32)
        {
            Trap(10);
            return;
        }
        State[XVM_REGISTER_WORD_OFFSET + destination] = foldedValue;
    }
    else
    {
        if ((selector != 1 && selector != 2) || sourceB >= XVM_REGISTER_COUNT || sourceC >= XVM_REGISTER_COUNT)
        {
            Trap(10);
            return;
        }
        uint valueB = selector == 1 ? foldedValue : State[XVM_REGISTER_WORD_OFFSET + sourceB];
        uint valueC = selector == 2 ? foldedValue : State[XVM_REGISTER_WORD_OFFSET + sourceC];
        if (opcode == 3)
        {
            State[XVM_REGISTER_WORD_OFFSET + destination] = valueB + valueC;
        }
        else if (opcode == 4)
        {
            State[XVM_REGISTER_WORD_OFFSET + destination] = valueB ^ valueC;
        }
        else if (opcode == 5)
        {
            State[XVM_REGISTER_WORD_OFFSET + destination] = valueB * valueC;
        }
        else if (opcode == 9)
        {
            State[XVM_REGISTER_WORD_OFFSET + destination] = valueB == valueC ? 1 : 0;
        }
        else
        {
            Trap(10);
            return;
        }
    }
    State[XVM_PC_WORD_OFFSET] = pc + 1;
    succeeded = 1;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0 || State[0] != 0 || State[4] != 0)
    {
        return;
    }
    uint instructionCount = Params[0];
    uint fuelLimit = Params[1];
    uint outputWords = Params[2];
    uint memoryWords = Params[3];
    uint epochInstructionBudget = Params[4];
    uint maxEpochs = Params[5];
    uint recordCount = Params[6];
    uint recordWords = Params[7];
    uint boundInstructionCount = Params[8];
    uint status = State[XVM_STATUS_WORD_OFFSET];
    State[3] = outputWords;
    State[5] = instructionCount;
    State[6] = memoryWords;
    State[7] = fuelLimit;
    if (instructionCount < 2 || instructionCount > XVM_MAX_INSTRUCTIONS ||
        boundInstructionCount != instructionCount || fuelLimit == 0 || fuelLimit > XVM_MAX_FUEL ||
        outputWords != XVM_OUTPUT_WORDS || memoryWords != XVM_MEMORY_WORDS ||
        epochInstructionBudget == 0 || epochInstructionBudget > XVM_MAX_EPOCH_INSTRUCTIONS ||
        maxEpochs == 0 || maxEpochs > XVM_MAX_EPOCHS || recordCount == 0 ||
        recordCount >= instructionCount || recordWords != XVM_TRACE_WORDS_PER_RECORD ||
        (status != XVM_STATUS_READY && status != XVM_STATUS_YIELDED))
    {
        Trap(10);
        return;
    }
    uint pc = State[XVM_PC_WORD_OFFSET];
    uint epochSequence = State[XVM_EPOCH_WORD_OFFSET];
    if (pc >= instructionCount || epochSequence >= maxEpochs || State[XVM_EXECUTED_RECORD_WORD_OFFSET] >= recordCount)
    {
        Trap(10);
        return;
    }
    State[XVM_STATUS_WORD_OFFSET] = XVM_STATUS_RUNNING;
    State[XVM_EPOCH_WORD_OFFSET] = epochSequence + 1;
    uint sourceSteps = 0;
    [loop]
    while (sourceSteps < epochInstructionBudget)
    {
        if (pc >= instructionCount)
        {
            Trap(10);
            return;
        }
        uint traceBase = pc * XVM_TRACE_WORDS_PER_RECORD;
        uint kind = Microtrace[traceBase];
        uint sourcePc = Microtrace[traceBase + 1];
        uint sourceSpan = Microtrace[traceBase + 2];
        uint fuelDebit = Microtrace[traceBase + 3];
        uint flags = Microtrace[traceBase + 14];
        uint reserved = Microtrace[traceBase + 15];
        if (sourcePc != pc || sourceSpan == 0 || sourceSpan > 2 || fuelDebit != sourceSpan ||
            sourceSteps + sourceSpan > epochInstructionBudget || pc + sourceSpan > instructionCount || reserved != 0 ||
            !InstructionMatches(traceBase, 4, pc) ||
            (sourceSpan == 2 && !InstructionMatches(traceBase, 8, pc + 1)))
        {
            Trap(10);
            return;
        }
        if ((kind == XVM_TRACE_RECORD_SINGLE && (sourceSpan != 1 || flags != 0)) ||
            (kind == XVM_TRACE_RECORD_PAIR && (sourceSpan != 2 || flags != 0)) ||
            (kind == XVM_TRACE_RECORD_CONSTANT_FOLD_PAIR &&
                (sourceSpan != 2 || flags != XVM_TRACE_FLAG_CONSTANT_PROPAGATED)) ||
            kind > XVM_TRACE_RECORD_CONSTANT_FOLD_PAIR)
        {
            Trap(10);
            return;
        }
        uint fuel = State[2];
        if (sourceSpan > fuelLimit || fuel > fuelLimit - sourceSpan)
        {
            Trap(1);
            return;
        }
        State[2] = fuel + 1;
        State[XVM_PC_WORD_OFFSET] = pc;
        uint firstSucceeded;
        ExecuteInstruction(Microtrace[traceBase + 4], Microtrace[traceBase + 5],
            Microtrace[traceBase + 6], Microtrace[traceBase + 7], pc, instructionCount, firstSucceeded);
        if (firstSucceeded == 0)
        {
            return;
        }
        if (State[XVM_STATUS_WORD_OFFSET] == XVM_STATUS_HALTED)
        {
            State[XVM_EXECUTED_RECORD_WORD_OFFSET] += 1;
            return;
        }
        if (sourceSpan == 2)
        {
            State[2] = fuel + 2;
            State[XVM_PC_WORD_OFFSET] = pc + 1;
            if (kind == XVM_TRACE_RECORD_CONSTANT_FOLD_PAIR)
            {
                uint firstOpcode = Microtrace[traceBase + 4];
                uint constantRegister = Microtrace[traceBase + 5];
                uint constantValue = Microtrace[traceBase + 6];
                uint selector = Microtrace[traceBase + 13];
                uint secondOpcode = Microtrace[traceBase + 8];
                bool foldValid = firstOpcode == 1 && constantRegister < XVM_REGISTER_COUNT &&
                    Microtrace[traceBase + 7] == 0;
                if (selector == 1)
                {
                    foldValid = foldValid && Microtrace[traceBase + 10] == constantRegister &&
                        Microtrace[traceBase + 12] == constantValue;
                }
                else if (selector == 2)
                {
                    foldValid = foldValid && Microtrace[traceBase + 11] == constantRegister &&
                        Microtrace[traceBase + 12] == constantValue;
                }
                else if (selector == 3)
                {
                    foldValid = foldValid && secondOpcode == 6 &&
                        Microtrace[traceBase + 10] == constantRegister && Microtrace[traceBase + 11] < 32 &&
                        Microtrace[traceBase + 12] == RotateLeft32(constantValue, Microtrace[traceBase + 11]);
                }
                else
                {
                    foldValid = false;
                }
                if (!foldValid)
                {
                    Trap(10);
                    return;
                }
                uint foldedSucceeded;
                ExecuteFoldedSecond(traceBase, pc + 1, foldedSucceeded);
                if (foldedSucceeded == 0)
                {
                    if (State[4] == 0)
                    {
                        Trap(10);
                    }
                    return;
                }
            }
            else
            {
                uint secondSucceeded;
                ExecuteInstruction(Microtrace[traceBase + 8], Microtrace[traceBase + 9],
                    Microtrace[traceBase + 10], Microtrace[traceBase + 11], pc + 1, instructionCount, secondSucceeded);
                if (secondSucceeded == 0)
                {
                    return;
                }
            }
        }
        pc += sourceSpan;
        sourceSteps += sourceSpan;
        State[XVM_PC_WORD_OFFSET] = pc;
        State[XVM_EXECUTED_RECORD_WORD_OFFSET] += 1;
    }
    State[XVM_STATUS_WORD_OFFSET] = XVM_STATUS_YIELDED;
}
