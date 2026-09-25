StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float4 v = Input[dispatchThreadId.x];
    for (uint i = 0; i < 32; ++i)
    {
        v = mad(v, float4(1.0001f, 0.9997f, 1.0003f, 0.5f), float4(0.01f, 0.02f, 0.03f, 0.0f));
    }
    Output[dispatchThreadId.x] = v;
}
