StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
RWStructuredBuffer<uint> State : register(u0);

#define XVM_RESIDENT_REGISTER_COUNT 16
#define XVM_RESIDENT_MEMORY_WORDS 16
#define XVM_RESIDENT_OUTPUT_WORDS 4
#define XVM_RESIDENT_MAX_INSTRUCTIONS 32
#define XVM_RESIDENT_MAX_FUEL 64
#define XVM_RESIDENT_MAX_EPOCH_INSTRUCTIONS 8
#define XVM_RESIDENT_MAX_EPOCHS 4
#define XVM_RESIDENT_OUTPUT_WORD_OFFSET 8
#define XVM_RESIDENT_PC_WORD_OFFSET 12
#define XVM_RESIDENT_EPOCH_WORD_OFFSET 13
#define XVM_RESIDENT_STATUS_WORD_OFFSET 14
#define XVM_RESIDENT_REGISTER_WORD_OFFSET 16
#define XVM_RESIDENT_MEMORY_WORD_OFFSET 32

#define XVM_RESIDENT_STATUS_READY 0
#define XVM_RESIDENT_STATUS_RUNNING 1
#define XVM_RESIDENT_STATUS_YIELDED 2
#define XVM_RESIDENT_STATUS_HALTED 3
#define XVM_RESIDENT_STATUS_TRAPPED 4

uint RotateLeft32(uint value, uint shift)
{
    return shift == 0 ? value : ((value << shift) | (value >> (32 - shift)));
}

void Trap(uint code)
{
    State[4] = code;
    State[XVM_RESIDENT_STATUS_WORD_OFFSET] = XVM_RESIDENT_STATUS_TRAPPED;
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
    uint status = State[XVM_RESIDENT_STATUS_WORD_OFFSET];

    State[3] = outputWords;
    State[5] = instructionCount;
    State[6] = memoryWords;
    State[7] = fuelLimit;

    if (instructionCount < 2 || instructionCount > XVM_RESIDENT_MAX_INSTRUCTIONS ||
        fuelLimit == 0 || fuelLimit > XVM_RESIDENT_MAX_FUEL ||
        outputWords != XVM_RESIDENT_OUTPUT_WORDS ||
        memoryWords != XVM_RESIDENT_MEMORY_WORDS ||
        epochInstructionBudget == 0 || epochInstructionBudget > XVM_RESIDENT_MAX_EPOCH_INSTRUCTIONS ||
        maxEpochs == 0 || maxEpochs > XVM_RESIDENT_MAX_EPOCHS ||
        (status != XVM_RESIDENT_STATUS_READY && status != XVM_RESIDENT_STATUS_YIELDED))
    {
        Trap(10);
        return;
    }

    uint pc = State[XVM_RESIDENT_PC_WORD_OFFSET];
    uint epochSequence = State[XVM_RESIDENT_EPOCH_WORD_OFFSET];
    if (pc >= instructionCount || epochSequence >= maxEpochs)
    {
        Trap(10);
        return;
    }

    State[XVM_RESIDENT_STATUS_WORD_OFFSET] = XVM_RESIDENT_STATUS_RUNNING;
    State[XVM_RESIDENT_EPOCH_WORD_OFFSET] = epochSequence + 1;

    [loop]
    for (uint step = 0; step < epochInstructionBudget; ++step)
    {
        uint fuel = State[2];
        if (fuel >= fuelLimit)
        {
            Trap(1);
            return;
        }
        if (pc >= instructionCount)
        {
            Trap(10);
            return;
        }
        State[2] = fuel + 1;
        State[XVM_RESIDENT_PC_WORD_OFFSET] = pc;

        uint base = pc * 4;
        uint opcode = Program[base];
        uint a = Program[base + 1];
        uint b = Program[base + 2];
        uint c = Program[base + 3];

        if (opcode == 0)
        {
            if (a != 0 || b != 0 || c != 0 || pc + 1 != instructionCount)
            {
                Trap(10);
            }
            else
            {
                State[0] = 1;
                State[XVM_RESIDENT_STATUS_WORD_OFFSET] = XVM_RESIDENT_STATUS_HALTED;
            }
            return;
        }
        if (opcode == 1 && a < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] = b;
        }
        else if (opcode == 2 && a < XVM_RESIDENT_REGISTER_COUNT && b < 11)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] = Inputs[b];
        }
        else if (opcode == 3 && a < XVM_RESIDENT_REGISTER_COUNT && b < XVM_RESIDENT_REGISTER_COUNT && c < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] =
                State[XVM_RESIDENT_REGISTER_WORD_OFFSET + b] + State[XVM_RESIDENT_REGISTER_WORD_OFFSET + c];
        }
        else if (opcode == 4 && a < XVM_RESIDENT_REGISTER_COUNT && b < XVM_RESIDENT_REGISTER_COUNT && c < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] =
                State[XVM_RESIDENT_REGISTER_WORD_OFFSET + b] ^ State[XVM_RESIDENT_REGISTER_WORD_OFFSET + c];
        }
        else if (opcode == 5 && a < XVM_RESIDENT_REGISTER_COUNT && b < XVM_RESIDENT_REGISTER_COUNT && c < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] =
                State[XVM_RESIDENT_REGISTER_WORD_OFFSET + b] * State[XVM_RESIDENT_REGISTER_WORD_OFFSET + c];
        }
        else if (opcode == 6 && a < XVM_RESIDENT_REGISTER_COUNT && b < XVM_RESIDENT_REGISTER_COUNT && c < 32)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] = RotateLeft32(State[XVM_RESIDENT_REGISTER_WORD_OFFSET + b], c);
        }
        else if (opcode == 7 && a < XVM_RESIDENT_REGISTER_COUNT && (b & 3) == 0 && b < XVM_RESIDENT_MEMORY_WORDS * 4)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] = State[XVM_RESIDENT_MEMORY_WORD_OFFSET + (b / 4)];
        }
        else if (opcode == 8 && a < XVM_RESIDENT_REGISTER_COUNT && (b & 3) == 0 && b < XVM_RESIDENT_MEMORY_WORDS * 4)
        {
            State[XVM_RESIDENT_MEMORY_WORD_OFFSET + (b / 4)] = State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a];
        }
        else if (opcode == 9 && a < XVM_RESIDENT_REGISTER_COUNT && b < XVM_RESIDENT_REGISTER_COUNT && c < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] =
                State[XVM_RESIDENT_REGISTER_WORD_OFFSET + b] == State[XVM_RESIDENT_REGISTER_WORD_OFFSET + c] ? 1 : 0;
        }
        else if (opcode == 13 && a < XVM_RESIDENT_REGISTER_COUNT && (b & 3) == 0 && b < XVM_RESIDENT_OUTPUT_WORDS * 4)
        {
            State[XVM_RESIDENT_OUTPUT_WORD_OFFSET + (b / 4)] = State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a];
        }
        else if (opcode == 14 && a < XVM_RESIDENT_REGISTER_COUNT)
        {
            State[1] = State[XVM_RESIDENT_REGISTER_WORD_OFFSET + a] == 0 ? 0 : 1;
        }
        else
        {
            Trap(10);
            return;
        }

        pc += 1;
        State[XVM_RESIDENT_PC_WORD_OFFSET] = pc;
    }

    State[XVM_RESIDENT_STATUS_WORD_OFFSET] = XVM_RESIDENT_STATUS_YIELDED;
}
