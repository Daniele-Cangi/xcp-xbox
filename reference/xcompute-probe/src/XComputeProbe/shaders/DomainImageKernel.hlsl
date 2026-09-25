StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    float4 v = Input[dispatchThreadId.x];
    for (uint i = 0; i < 12; ++i)
    {
        float luma = v.x * 0.299f + v.y * 0.587f + v.z * 0.114f;
        float blur = (v.x + v.y + v.z + v.w) * 0.25f;
        float edge = (v.x - v.y) * 0.5f + (v.z - v.w) * 0.25f;
        float sharpen = luma * 1.5f - blur * 0.35f + 0.01f;
        v = float4(
            sharpen,
            blur * 0.92f + edge * 0.08f + 0.02f,
            luma * 0.85f + edge * 0.15f + 0.03f,
            v.w * 0.60f + luma * 0.40f);
    }
    Output[dispatchThreadId.x] = v;
}
