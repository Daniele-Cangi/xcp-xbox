StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> Output : register(u0);

uint Mix32(uint value)
{
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

float UnitFromHash(uint value)
{
    return (float)(value & 0xffffu) / 65535.0f;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint h = dispatchThreadId.x + 1u;
    for (uint i = 0; i < 8; ++i)
    {
        h = Mix32(h + 0x9e3779b9u + (i * 0x85ebca6bu));
    }

    uint h1 = h;
    uint h2 = Mix32(h1 + 0x7f4a7c15u);
    uint h3 = Mix32(h2 + 0x94d049bbu);
    uint h4 = Mix32(h3 + 0x2545f491u);

    float4 seed = Input[dispatchThreadId.x];
    Output[dispatchThreadId.x] = float4(
        UnitFromHash(h1) + seed.w,
        UnitFromHash(h2) + seed.w * 0.5f,
        UnitFromHash(h3) + seed.w * 0.25f,
        UnitFromHash(h4));
}
