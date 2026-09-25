StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
RWStructuredBuffer<uint> Field : register(u0);

#define WORLD_WIDTH 1024
#define WORLD_HEIGHT 1024
#define WORLD_LANE_COUNT 1048576
#define WORLD_WORKGROUP_X 8
#define WORLD_WORKGROUP_Y 8
#define WORLD_STATIC_FUEL 16
#define WORLD_PROGRAM_WORDS 64
#define WORLD_PLAN_VERSION 11

bool ProgramSignatureMatches()
{
    return Program[0] == 2 && Program[1] == 0 && Program[4] == 1 &&
        Program[5] == 1 && Program[6] == 374761393u &&
        Program[8] == 5 && Program[9] == 2 && Program[10] == 13 &&
        Program[12] == 1 && Program[13] == 3 && Program[14] == 668265263u &&
        Program[16] == 5 && Program[17] == 4 && Program[18] == 14 &&
        Program[20] == 4 && Program[24] == 4 && Program[28] == 6 &&
        Program[29] == 7 && Program[30] == 6 && Program[31] == 13 &&
        Program[32] == 3 && Program[36] == 4 && Program[40] == 13 &&
        Program[41] == 9 && Program[44] == 15 && Program[45] == 13 &&
        Program[48] == 0 && Program[52] == 1 && Program[53] == 10 &&
        Program[54] == 1 && Program[56] == 14 && Program[57] == 10 &&
        Program[60] == 16;
}

uint RotateLeft13(uint value)
{
    return (value << 13) | (value >> 19);
}

[numthreads(WORLD_WORKGROUP_X, WORLD_WORKGROUP_Y, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= WORLD_WIDTH || dispatchThreadId.y >= WORLD_HEIGHT ||
        dispatchThreadId.z != 0)
    {
        return;
    }

    uint laneId = dispatchThreadId.x + WORLD_WIDTH * dispatchThreadId.y;
    bool planMatches = Params[0] == WORLD_WIDTH && Params[1] == WORLD_HEIGHT &&
        Params[2] == WORLD_LANE_COUNT && Params[3] == WORLD_WORKGROUP_X &&
        Params[4] == WORLD_WORKGROUP_Y && Params[5] == WORLD_STATIC_FUEL &&
        Params[6] == WORLD_PROGRAM_WORDS && Params[7] == WORLD_PLAN_VERSION;
    if (!planMatches || !ProgramSignatureMatches())
    {
        Field[laneId] = 0xffffffffu;
        return;
    }

    uint xMix = dispatchThreadId.x * 374761393u;
    uint yMix = dispatchThreadId.y * 668265263u;
    uint combined = xMix ^ yMix;
    uint seeded = combined ^ Inputs[0];
    uint rotated = RotateLeft13(seeded);
    Field[laneId] = (rotated + dispatchThreadId.x) ^ dispatchThreadId.y;
}
