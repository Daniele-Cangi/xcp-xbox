StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
RWStructuredBuffer<uint> State : register(u0);

#define XVM_GENERAL_REGISTER_COUNT 16
#define XVM_GENERAL_INPUT_WORDS 16
#define XVM_GENERAL_MEMORY_WORDS 16
#define XVM_GENERAL_OUTPUT_WORDS 4
#define XVM_GENERAL_MAX_INSTRUCTIONS 32
#define XVM_GENERAL_MAX_FUEL 64
#define XVM_GENERAL_RESULT_OUTPUT_WORD_OFFSET 8
#define XVM_GENERAL_REGISTER_WORD_OFFSET 16
#define XVM_GENERAL_MEMORY_WORD_OFFSET 32

uint RotateLeft32(uint value, uint shift)
{
    return shift == 0 ? value : ((value << shift) | (value >> (32 - shift)));
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
    uint pc = Params[4];

    State[3] = outputWords;
    State[5] = instructionCount;
    State[6] = memoryWords;
    State[7] = fuelLimit;

    if (instructionCount < 2 || instructionCount > XVM_GENERAL_MAX_INSTRUCTIONS ||
        fuelLimit == 0 || fuelLimit > XVM_GENERAL_MAX_FUEL ||
        outputWords != XVM_GENERAL_OUTPUT_WORDS ||
        memoryWords != XVM_GENERAL_MEMORY_WORDS ||
        pc >= instructionCount)
    {
        State[4] = 10;
        return;
    }

    uint fuel = State[2];
    if (fuel >= fuelLimit)
    {
        State[4] = 1;
        return;
    }
    State[2] = fuel + 1;

    uint base = pc * 4;
    uint opcode = Program[base];
    uint a = Program[base + 1];
    uint b = Program[base + 2];
    uint c = Program[base + 3];

    if (opcode == 0)
    {
        if (a != 0 || b != 0 || c != 0 || pc + 1 != instructionCount)
        {
            State[4] = 10;
        }
        else
        {
            State[0] = 1;
        }
        return;
    }
    if (opcode == 1 && a < XVM_GENERAL_REGISTER_COUNT)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] = b;
    }
    else if (opcode == 2 && a < XVM_GENERAL_REGISTER_COUNT && b < 11)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] = Inputs[b];
    }
    else if (opcode == 3 && a < XVM_GENERAL_REGISTER_COUNT && b < XVM_GENERAL_REGISTER_COUNT && c < XVM_GENERAL_REGISTER_COUNT)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] =
            State[XVM_GENERAL_REGISTER_WORD_OFFSET + b] + State[XVM_GENERAL_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 4 && a < XVM_GENERAL_REGISTER_COUNT && b < XVM_GENERAL_REGISTER_COUNT && c < XVM_GENERAL_REGISTER_COUNT)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] =
            State[XVM_GENERAL_REGISTER_WORD_OFFSET + b] ^ State[XVM_GENERAL_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 5 && a < XVM_GENERAL_REGISTER_COUNT && b < XVM_GENERAL_REGISTER_COUNT && c < XVM_GENERAL_REGISTER_COUNT)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] =
            State[XVM_GENERAL_REGISTER_WORD_OFFSET + b] * State[XVM_GENERAL_REGISTER_WORD_OFFSET + c];
    }
    else if (opcode == 6 && a < XVM_GENERAL_REGISTER_COUNT && b < XVM_GENERAL_REGISTER_COUNT && c < 32)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] = RotateLeft32(State[XVM_GENERAL_REGISTER_WORD_OFFSET + b], c);
    }
    else if (opcode == 7 && a < XVM_GENERAL_REGISTER_COUNT && (b & 3) == 0 && b < XVM_GENERAL_MEMORY_WORDS * 4)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] = State[XVM_GENERAL_MEMORY_WORD_OFFSET + (b / 4)];
    }
    else if (opcode == 8 && a < XVM_GENERAL_REGISTER_COUNT && (b & 3) == 0 && b < XVM_GENERAL_MEMORY_WORDS * 4)
    {
        State[XVM_GENERAL_MEMORY_WORD_OFFSET + (b / 4)] = State[XVM_GENERAL_REGISTER_WORD_OFFSET + a];
    }
    else if (opcode == 9 && a < XVM_GENERAL_REGISTER_COUNT && b < XVM_GENERAL_REGISTER_COUNT && c < XVM_GENERAL_REGISTER_COUNT)
    {
        State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] =
            State[XVM_GENERAL_REGISTER_WORD_OFFSET + b] == State[XVM_GENERAL_REGISTER_WORD_OFFSET + c] ? 1 : 0;
    }
    else if (opcode == 13 && a < XVM_GENERAL_REGISTER_COUNT && (b & 3) == 0 && b < XVM_GENERAL_OUTPUT_WORDS * 4)
    {
        State[XVM_GENERAL_RESULT_OUTPUT_WORD_OFFSET + (b / 4)] = State[XVM_GENERAL_REGISTER_WORD_OFFSET + a];
    }
    else if (opcode == 14 && a < XVM_GENERAL_REGISTER_COUNT)
    {
        State[1] = State[XVM_GENERAL_REGISTER_WORD_OFFSET + a] == 0 ? 0 : 1;
    }
    else
    {
        State[4] = 10;
    }
}
