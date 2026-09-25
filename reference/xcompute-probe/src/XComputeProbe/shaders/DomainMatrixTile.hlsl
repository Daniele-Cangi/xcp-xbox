StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float4 v = Input[dispatchThreadId.x];
    for (uint i = 0; i < 16; ++i)
    {
        v = float4(
            dot(v, float4(0.92f, 0.04f, -0.02f, 0.01f)) + 0.010f,
            dot(v, float4(0.03f, 0.91f, 0.05f, -0.01f)) + 0.020f,
            dot(v, float4(-0.02f, 0.06f, 0.89f, 0.04f)) + 0.030f,
            dot(v, float4(0.01f, -0.03f, 0.04f, 0.94f)) + 0.040f);
    }
    Output[dispatchThreadId.x] = v;
}
