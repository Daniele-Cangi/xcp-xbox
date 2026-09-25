StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
RWStructuredBuffer<uint> State : register(u0);

#define XVM_REGISTER_COUNT 16
#define XVM_INSTRUCTION_COUNT 9
#define XVM_MEMORY_WORDS 16
#define XVM_OUTPUT_WORDS 4
#define XVM_FUEL_LIMIT 16
#define XVM_EPOCH_INSTRUCTIONS 8
#define XVM_MAX_EPOCHS 2
#define XVM_MIN_LANE_COUNT 2
#define XVM_MAX_LANE_COUNT 4096
#define XVM_WORKGROUP_X 8
#define XVM_WORKGROUP_Y 8
#define XVM_WORKGROUP_Z 1
#define XVM_STATE_WORDS_PER_LANE 128
#define XVM_INPUT_WORDS_PER_LANE 16
#define XVM_CAPABILITY_FLAGS 8
#define XVM_MAX_CALL_DEPTH 1

#define STATE_HALTED 0
#define STATE_CONTROL 1
#define STATE_FUEL 2
#define STATE_OUTPUT_COUNT 3
#define STATE_FAULT_CODE 4
#define STATE_INSTRUCTION_COUNT 5
#define STATE_MEMORY_COUNT 6
#define STATE_FUEL_LIMIT 7
#define STATE_OUTPUT 8
#define STATE_PC 12
#define STATE_EPOCH 13
#define STATE_STATUS 14
#define STATE_CAPABILITIES 15
#define STATE_REGISTERS 16
#define STATE_LANE_ID 48
#define STATE_LANE_COUNT 49
#define STATE_GRID_X 50
#define STATE_GRID_Y 51
#define STATE_GRID_Z 52
#define STATE_WORKGROUP_X 53
#define STATE_WORKGROUP_Y 54
#define STATE_WORKGROUP_Z 55
#define STATE_STRIDE 56
#define STATE_INPUT_STRIDE 57
#define STATE_CALL_DEPTH 58
#define STATE_MAX_CALL_DEPTH 59
#define STATE_RETURN_PC 60
#define STATE_LOOP_DEPTH 76
#define STATE_MAX_LOOP_DEPTH 77
#define STATE_TRAP_STATUS 78
#define STATE_TRAP_CODE 79
#define STATE_TRAP_PC 80
#define STATE_TRAP_RECOVERY_PC 81
#define STATE_TRAP_OCCURRENCE_COUNT 82
#define STATE_PLAN_VERSION 83
#define STATE_STATIC_WORST_CASE_FUEL 84
#define STATE_REQUIRED_EPOCHS 85

#define STATUS_READY 0
#define STATUS_RUNNING 1
#define STATUS_YIELDED 2
#define STATUS_HALTED 3
#define STATUS_FAULTED 4

void Fault(uint stateBase, uint code)
{
    State[stateBase + STATE_FAULT_CODE] = code;
    State[stateBase + STATE_STATUS] = STATUS_FAULTED;
}

[numthreads(XVM_WORKGROUP_X, XVM_WORKGROUP_Y, XVM_WORKGROUP_Z)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint laneCount = Params[6];
    uint gridX = Params[7];
    uint gridY = Params[8];
    uint gridZ = Params[9];
    uint gridXY = gridX * gridY;
    bool topologyValid = laneCount >= XVM_MIN_LANE_COUNT && laneCount <= XVM_MAX_LANE_COUNT &&
        gridX >= 1 && gridX <= XVM_MAX_LANE_COUNT &&
        gridY >= 1 && gridY <= XVM_MAX_LANE_COUNT &&
        gridZ >= 1 && gridZ <= XVM_MAX_LANE_COUNT &&
        gridXY >= 1 && gridXY <= laneCount &&
        laneCount % gridXY == 0 && laneCount / gridXY == gridZ &&
        Params[10] == XVM_WORKGROUP_X && Params[11] == XVM_WORKGROUP_Y &&
        Params[12] == XVM_WORKGROUP_Z;
    if (!topologyValid || dispatchThreadId.x >= gridX || dispatchThreadId.y >= gridY ||
        dispatchThreadId.z >= gridZ)
    {
        return;
    }

    uint laneId = dispatchThreadId.x + gridX *
        (dispatchThreadId.y + gridY * dispatchThreadId.z);
    if (laneId >= laneCount)
    {
        return;
    }

    uint stateBase = laneId * XVM_STATE_WORDS_PER_LANE;
    if (State[stateBase + STATE_HALTED] != 0 || State[stateBase + STATE_FAULT_CODE] != 0)
    {
        return;
    }

    uint status = State[stateBase + STATE_STATUS];
    State[stateBase + STATE_OUTPUT_COUNT] = XVM_OUTPUT_WORDS;
    State[stateBase + STATE_INSTRUCTION_COUNT] = XVM_INSTRUCTION_COUNT;
    State[stateBase + STATE_MEMORY_COUNT] = XVM_MEMORY_WORDS;
    State[stateBase + STATE_FUEL_LIMIT] = XVM_FUEL_LIMIT;

    if (Params[0] != XVM_INSTRUCTION_COUNT || Params[1] != XVM_FUEL_LIMIT ||
        Params[2] != XVM_OUTPUT_WORDS || Params[3] != XVM_MEMORY_WORDS ||
        Params[4] != XVM_EPOCH_INSTRUCTIONS || Params[5] != XVM_MAX_EPOCHS ||
        Params[13] != XVM_STATE_WORDS_PER_LANE ||
        Params[14] != XVM_INPUT_WORDS_PER_LANE || Params[15] != XVM_CAPABILITY_FLAGS ||
        State[stateBase + STATE_LANE_ID] != laneId ||
        State[stateBase + STATE_LANE_COUNT] != laneCount ||
        State[stateBase + STATE_GRID_X] != gridX ||
        State[stateBase + STATE_GRID_Y] != gridY ||
        State[stateBase + STATE_GRID_Z] != gridZ ||
        State[stateBase + STATE_WORKGROUP_X] != XVM_WORKGROUP_X ||
        State[stateBase + STATE_WORKGROUP_Y] != XVM_WORKGROUP_Y ||
        State[stateBase + STATE_WORKGROUP_Z] != XVM_WORKGROUP_Z ||
        State[stateBase + STATE_STRIDE] != XVM_STATE_WORDS_PER_LANE ||
        State[stateBase + STATE_INPUT_STRIDE] != XVM_INPUT_WORDS_PER_LANE ||
        State[stateBase + STATE_CAPABILITIES] != XVM_CAPABILITY_FLAGS ||
        State[stateBase + STATE_MAX_CALL_DEPTH] != XVM_MAX_CALL_DEPTH ||
        State[stateBase + STATE_MAX_LOOP_DEPTH] != 0 ||
        State[stateBase + STATE_LOOP_DEPTH] != 0 ||
        State[stateBase + STATE_TRAP_STATUS] != 0 ||
        State[stateBase + STATE_TRAP_CODE] != 0 ||
        State[stateBase + STATE_TRAP_PC] != 0 ||
        State[stateBase + STATE_TRAP_RECOVERY_PC] != 0 ||
        State[stateBase + STATE_TRAP_OCCURRENCE_COUNT] != 0 ||
        State[stateBase + STATE_PLAN_VERSION] != 10 ||
        State[stateBase + STATE_STATIC_WORST_CASE_FUEL] != 9 ||
        State[stateBase + STATE_REQUIRED_EPOCHS] != 2 ||
        State[stateBase + STATE_REGISTERS + 12] != laneId ||
        State[stateBase + STATE_REGISTERS + 13] != dispatchThreadId.x ||
        State[stateBase + STATE_REGISTERS + 14] != dispatchThreadId.y ||
        State[stateBase + STATE_REGISTERS + 15] != dispatchThreadId.z ||
        (status != STATUS_READY && status != STATUS_YIELDED))
    {
        Fault(stateBase, 10);
        return;
    }

    uint pc = State[stateBase + STATE_PC];
    uint epoch = State[stateBase + STATE_EPOCH];
    if (pc >= XVM_INSTRUCTION_COUNT || epoch >= XVM_MAX_EPOCHS)
    {
        Fault(stateBase, 10);
        return;
    }

    State[stateBase + STATE_STATUS] = STATUS_RUNNING;
    State[stateBase + STATE_EPOCH] = epoch + 1;

    [loop]
    for (uint step = 0; step < XVM_EPOCH_INSTRUCTIONS; ++step)
    {
        uint fuel = State[stateBase + STATE_FUEL];
        if (fuel >= XVM_FUEL_LIMIT || pc >= XVM_INSTRUCTION_COUNT)
        {
            Fault(stateBase, fuel >= XVM_FUEL_LIMIT ? 1 : 10);
            return;
        }
        State[stateBase + STATE_FUEL] = fuel + 1;
        State[stateBase + STATE_PC] = pc;

        uint base = pc * 4;
        uint opcode = Program[base];
        uint a = Program[base + 1];
        uint b = Program[base + 2];
        uint c = Program[base + 3];

        if (opcode == 0 && a == 0 && b == 0 && c == 0 &&
            State[stateBase + STATE_CALL_DEPTH] == 0)
        {
            State[stateBase + STATE_HALTED] = 1;
            State[stateBase + STATE_PC] = XVM_INSTRUCTION_COUNT;
            State[stateBase + STATE_STATUS] = STATUS_HALTED;
            return;
        }
        else if (opcode == 1 && a < XVM_REGISTER_COUNT && c == 0)
        {
            State[stateBase + STATE_REGISTERS + a] = b;
            pc += 1;
        }
        else if (opcode == 13 && a < XVM_REGISTER_COUNT && (b & 3) == 0 &&
            b < XVM_OUTPUT_WORDS * 4 && c == 0)
        {
            State[stateBase + STATE_OUTPUT + (b / 4)] =
                State[stateBase + STATE_REGISTERS + a];
            pc += 1;
        }
        else if (opcode == 14 && a < XVM_REGISTER_COUNT && b == 0 && c == 0)
        {
            State[stateBase + STATE_CONTROL] =
                State[stateBase + STATE_REGISTERS + a] == 0 ? 0 : 1;
            pc += 1;
        }
        else if (opcode == 15 && a == 7 && b == 0 && c == 0 &&
            State[stateBase + STATE_CALL_DEPTH] == 0)
        {
            State[stateBase + STATE_RETURN_PC] = pc + 1;
            State[stateBase + STATE_CALL_DEPTH] = 1;
            pc = a;
        }
        else if (opcode == 16 && a == 0 && b == 0 && c == 0 &&
            State[stateBase + STATE_CALL_DEPTH] == 1)
        {
            uint returnPc = State[stateBase + STATE_RETURN_PC];
            if (returnPc >= XVM_INSTRUCTION_COUNT)
            {
                Fault(stateBase, 10);
                return;
            }
            State[stateBase + STATE_RETURN_PC] = 0;
            State[stateBase + STATE_CALL_DEPTH] = 0;
            pc = returnPc;
        }
        else
        {
            Fault(stateBase, 10);
            return;
        }

        if (pc >= XVM_INSTRUCTION_COUNT)
        {
            Fault(stateBase, 10);
            return;
        }
        State[stateBase + STATE_PC] = pc;
    }

    State[stateBase + STATE_STATUS] = STATUS_YIELDED;
}