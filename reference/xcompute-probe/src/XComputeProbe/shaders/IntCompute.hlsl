RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint x = dispatchThreadId.x + 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    Output[dispatchThreadId.x] = x;
}

