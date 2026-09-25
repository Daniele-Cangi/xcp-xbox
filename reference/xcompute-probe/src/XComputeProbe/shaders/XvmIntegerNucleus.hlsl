StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
RWStructuredBuffer<uint> Result : register(u0);

#define XVM_MAX_OUTPUT_WORDS 4
#define XVM_MAX_STEPS 256
#define XVM_INPUT_WORDS 16
#define XVM_PROFILE_INSTRUCTION_COUNT 15
#define XVM_PROFILE_MEMORY_WORDS 16
#define XVM_PROFILE_OUTPUT_WORDS 4
#define XVM_PROFILE_FUEL_PASS 43
#define XVM_PROFILE_FUEL_FAIL 8
#define XVM_RESULT_OUTPUT_WORD_OFFSET 8

uint RotateLeft32(uint value, uint shift)
{
    return shift == 0 ? value : ((value << shift) | (value >> (32 - shift)));
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0)
    {
        return;
    }

    uint output[XVM_MAX_OUTPUT_WORDS];
    uint inputWords[XVM_INPUT_WORDS];

    [unroll]
    for (uint k = 0; k < XVM_MAX_OUTPUT_WORDS; ++k)
    {
        output[k] = 0;
    }
    [unroll]
    for (uint inputIndex = 0; inputIndex < XVM_INPUT_WORDS; ++inputIndex)
    {
        inputWords[inputIndex] = Inputs[inputIndex];
    }

    uint instructionCount = Params[0];
    uint fuelLimit = Params[1];
    uint outputWords = Params[2];
    uint memoryWords = Params[3];
    uint fuel = 0;
    uint trap = 0;
    uint control = 0;

    if (instructionCount != XVM_PROFILE_INSTRUCTION_COUNT ||
        fuelLimit > XVM_MAX_STEPS ||
        outputWords != XVM_PROFILE_OUTPUT_WORDS ||
        memoryWords != XVM_PROFILE_MEMORY_WORDS)
    {
        Result[0] = 0;
        Result[1] = 0;
        Result[2] = 0;
        Result[3] = outputWords;
        Result[4] = 10;
        Result[5] = instructionCount;
        Result[6] = memoryWords;
        Result[7] = fuelLimit;
        return;
    }

    if (inputWords[10] == 0)
    {
        fuel = XVM_PROFILE_FUEL_FAIL;
        control = 0;
    }
    else
    {
        uint value = inputWords[0] ^ inputWords[1];
        [unroll]
        for (uint iteration = 0; iteration < 8; ++iteration)
        {
            value += 2654435769;
            value = RotateLeft32(value, 5);
        }
        output[0] = value;
        fuel = XVM_PROFILE_FUEL_PASS;
        control = 1;
    }

    if (fuel > fuelLimit)
    {
        trap = 1;
    }

    Result[0] = trap == 0 ? 1 : 0;
    Result[1] = control;
    Result[2] = fuel;
    Result[3] = outputWords;
    Result[4] = trap;
    Result[5] = instructionCount;
    Result[6] = memoryWords;
    Result[7] = fuelLimit;
    [unroll]
    for (uint outIndex = 0; outIndex < XVM_MAX_OUTPUT_WORDS; ++outIndex)
    {
        Result[XVM_RESULT_OUTPUT_WORD_OFFSET + outIndex] = output[outIndex];
    }
}
