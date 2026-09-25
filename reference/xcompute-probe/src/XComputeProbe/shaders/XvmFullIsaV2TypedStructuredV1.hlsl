StructuredBuffer<uint> Params : register(t0);
StructuredBuffer<uint> Inputs : register(t1);
StructuredBuffer<uint> Program : register(t2);
RWStructuredBuffer<uint> State : register(u0);

#define XVM_REGISTER_COUNT 16
#define XVM_MEMORY_WORDS 16
#define XVM_OUTPUT_WORDS 4
#define XVM_MAX_INSTRUCTIONS 32
#define XVM_MAX_FUEL 64
#define XVM_MAX_EPOCH_INSTRUCTIONS 8
#define XVM_MAX_EPOCHS 8
#define XVM_STATE_WORDS_PER_LANE 96
#define XVM_INPUT_WORDS_PER_LANE 16
#define XVM_CAPABILITY_FLAGS 7

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
#define STATE_MEMORY 32
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
#define STATE_TRAP_CODE 78
#define STATE_PLAN_VERSION 83
#define STATE_STATIC_WORST_CASE_FUEL 84
#define STATE_REQUIRED_EPOCHS 85

#define STATUS_READY 0
#define STATUS_RUNNING 1
#define STATUS_YIELDED 2
#define STATUS_HALTED 3
#define STATUS_FAULTED 4

uint RotateLeft32(uint value, uint shift)
{
    return shift == 0 ? value : ((value << shift) | (value >> (32 - shift)));
}

void Fault(uint stateBase, uint code)
{
    State[stateBase + STATE_FAULT_CODE] = code;
    State[stateBase + STATE_STATUS] = STATUS_FAULTED;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint laneCount = Params[6];
    uint gridX = Params[7];
    uint gridY = Params[8];
    uint gridZ = Params[9];
    if (dispatchThreadId.x >= gridX || dispatchThreadId.y >= gridY || dispatchThreadId.z >= gridZ)
    {
        return;
    }

    uint laneId = dispatchThreadId.x + gridX * (dispatchThreadId.y + gridY * dispatchThreadId.z);
    uint stateStride = Params[13];
    uint inputStride = Params[14];
    uint stateBase = laneId * stateStride;
    uint inputBase = laneId * inputStride;
    if (laneId >= laneCount || State[stateBase + STATE_HALTED] != 0 ||
        State[stateBase + STATE_FAULT_CODE] != 0)
    {
        return;
    }

    uint instructionCount = Params[0];
    uint fuelLimit = Params[1];
    uint outputWords = Params[2];
    uint memoryWords = Params[3];
    uint epochInstructionBudget = Params[4];
    uint maxEpochs = Params[5];
    uint capabilityFlags = Params[15];
    uint status = State[stateBase + STATE_STATUS];

    State[stateBase + STATE_OUTPUT_COUNT] = outputWords;
    State[stateBase + STATE_INSTRUCTION_COUNT] = instructionCount;
    State[stateBase + STATE_MEMORY_COUNT] = memoryWords;
    State[stateBase + STATE_FUEL_LIMIT] = fuelLimit;

    if (instructionCount < 4 || instructionCount > XVM_MAX_INSTRUCTIONS ||
        fuelLimit == 0 || fuelLimit > XVM_MAX_FUEL ||
        outputWords != XVM_OUTPUT_WORDS || memoryWords != XVM_MEMORY_WORDS ||
        epochInstructionBudget == 0 || epochInstructionBudget > XVM_MAX_EPOCH_INSTRUCTIONS ||
        maxEpochs == 0 || maxEpochs > XVM_MAX_EPOCHS ||
        laneCount != 1 || gridX != 1 || gridY != 1 || gridZ != 1 ||
        Params[10] != 1 || Params[11] != 1 || Params[12] != 1 ||
        stateStride != XVM_STATE_WORDS_PER_LANE || inputStride != XVM_INPUT_WORDS_PER_LANE ||
        capabilityFlags != XVM_CAPABILITY_FLAGS ||
        State[stateBase + STATE_LANE_ID] != laneId ||
        State[stateBase + STATE_LANE_COUNT] != laneCount ||
        State[stateBase + STATE_GRID_X] != gridX ||
        State[stateBase + STATE_GRID_Y] != gridY ||
        State[stateBase + STATE_GRID_Z] != gridZ ||
        State[stateBase + STATE_WORKGROUP_X] != 1 ||
        State[stateBase + STATE_WORKGROUP_Y] != 1 ||
        State[stateBase + STATE_WORKGROUP_Z] != 1 ||
        State[stateBase + STATE_STRIDE] != stateStride ||
        State[stateBase + STATE_INPUT_STRIDE] != inputStride ||
        State[stateBase + STATE_CAPABILITIES] != capabilityFlags ||
        State[stateBase + STATE_MAX_CALL_DEPTH] != 1 ||
        State[stateBase + STATE_LOOP_DEPTH] != 0 ||
        State[stateBase + STATE_MAX_LOOP_DEPTH] != 0 ||
        State[stateBase + STATE_TRAP_CODE] != 0 ||
        State[stateBase + STATE_PLAN_VERSION] != 2 ||
        State[stateBase + STATE_STATIC_WORST_CASE_FUEL] == 0 ||
        State[stateBase + STATE_STATIC_WORST_CASE_FUEL] > fuelLimit ||
        State[stateBase + STATE_REQUIRED_EPOCHS] == 0 ||
        State[stateBase + STATE_REQUIRED_EPOCHS] > maxEpochs ||
        (status != STATUS_READY && status != STATUS_YIELDED))
    {
        Fault(stateBase, 10);
        return;
    }

    uint pc = State[stateBase + STATE_PC];
    uint epochSequence = State[stateBase + STATE_EPOCH];
    if (pc >= instructionCount || epochSequence >= maxEpochs)
    {
        Fault(stateBase, 10);
        return;
    }

    State[stateBase + STATE_STATUS] = STATUS_RUNNING;
    State[stateBase + STATE_EPOCH] = epochSequence + 1;

    [loop]
    for (uint step = 0; step < epochInstructionBudget; ++step)
    {
        uint fuel = State[stateBase + STATE_FUEL];
        if (fuel >= fuelLimit || pc >= instructionCount)
        {
            Fault(stateBase, fuel >= fuelLimit ? 1 : 10);
            return;
        }
        State[stateBase + STATE_FUEL] = fuel + 1;
        State[stateBase + STATE_PC] = pc;

        uint base = pc * 4;
        uint opcode = Program[base];
        uint a = Program[base + 1];
        uint b = Program[base + 2];
        uint c = Program[base + 3];

        if (opcode == 0)
        {
            if (a != 0 || b != 0 || c != 0 || State[stateBase + STATE_CALL_DEPTH] != 0)
            {
                Fault(stateBase, 10);
            }
            else
            {
                State[stateBase + STATE_HALTED] = 1;
                State[stateBase + STATE_PC] = instructionCount;
                State[stateBase + STATE_STATUS] = STATUS_HALTED;
            }
            return;
        }
        if (opcode == 1 && a < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] = b;
            pc += 1;
        }
        else if (opcode == 2 && a < XVM_REGISTER_COUNT && b < 11)
        {
            State[stateBase + STATE_REGISTERS + a] = Inputs[inputBase + b];
            pc += 1;
        }
        else if (opcode == 3 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                State[stateBase + STATE_REGISTERS + b] + State[stateBase + STATE_REGISTERS + c];
            pc += 1;
        }
        else if (opcode == 4 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                State[stateBase + STATE_REGISTERS + b] ^ State[stateBase + STATE_REGISTERS + c];
            pc += 1;
        }
        else if (opcode == 5 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                State[stateBase + STATE_REGISTERS + b] * State[stateBase + STATE_REGISTERS + c];
            pc += 1;
        }
        else if (opcode == 6 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < 32)
        {
            State[stateBase + STATE_REGISTERS + a] = RotateLeft32(State[stateBase + STATE_REGISTERS + b], c);
            pc += 1;
        }
        else if (opcode == 7 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_MEMORY_WORDS * 4)
        {
            State[stateBase + STATE_REGISTERS + a] = State[stateBase + STATE_MEMORY + (b / 4)];
            pc += 1;
        }
        else if (opcode == 8 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_MEMORY_WORDS * 4)
        {
            State[stateBase + STATE_MEMORY + (b / 4)] = State[stateBase + STATE_REGISTERS + a];
            pc += 1;
        }
        else if (opcode == 9 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                State[stateBase + STATE_REGISTERS + b] == State[stateBase + STATE_REGISTERS + c] ? 1 : 0;
            pc += 1;
        }
        else if (opcode == 13 && a < XVM_REGISTER_COUNT && (b & 3) == 0 && b < XVM_OUTPUT_WORDS * 4)
        {
            State[stateBase + STATE_OUTPUT + (b / 4)] = State[stateBase + STATE_REGISTERS + a];
            pc += 1;
        }
        else if (opcode == 14 && a < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_CONTROL] = State[stateBase + STATE_REGISTERS + a] == 0 ? 0 : 1;
            pc += 1;
        }
        else if (opcode == 15 && b == 0 && c == 0 && a < instructionCount && State[stateBase + STATE_CALL_DEPTH] == 0)
        {
            State[stateBase + STATE_RETURN_PC] = pc + 1;
            State[stateBase + STATE_CALL_DEPTH] = 1;
            pc = a;
        }
        else if (opcode == 16 && a == 0 && b == 0 && c == 0 && State[stateBase + STATE_CALL_DEPTH] == 1)
        {
            pc = State[stateBase + STATE_RETURN_PC];
            State[stateBase + STATE_RETURN_PC] = 0;
            State[stateBase + STATE_CALL_DEPTH] = 0;
        }
        else if (opcode == 17 && a < XVM_REGISTER_COUNT && b <= c && c < instructionCount)
        {
            pc = State[stateBase + STATE_REGISTERS + a] == 0 ? (b < c ? b + 1 : c + 1) : pc + 1;
        }
        else if (opcode == 18 && b == 0 && c == 0 && a < instructionCount)
        {
            pc = a + 1;
        }
        else if (opcode == 19 && a == 0 && b == 0 && c == 0)
        {
            pc += 1;
        }
        else if (opcode == 20 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                State[stateBase + STATE_REGISTERS + b] < State[stateBase + STATE_REGISTERS + c] ? 1 : 0;
            pc += 1;
        }
        else if (opcode == 21 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] =
                asint(State[stateBase + STATE_REGISTERS + b]) < asint(State[stateBase + STATE_REGISTERS + c]) ? 1 : 0;
            pc += 1;
        }
        else if (opcode == 22 && a < XVM_REGISTER_COUNT && b < XVM_REGISTER_COUNT && c < XVM_REGISTER_COUNT)
        {
            State[stateBase + STATE_REGISTERS + a] = State[stateBase + STATE_REGISTERS + a] != 0
                ? State[stateBase + STATE_REGISTERS + b]
                : State[stateBase + STATE_REGISTERS + c];
            pc += 1;
        }
        else
        {
            Fault(stateBase, 10);
            return;
        }

        if (pc >= instructionCount)
        {
            Fault(stateBase, 10);
            return;
        }
        State[stateBase + STATE_PC] = pc;
    }

    State[stateBase + STATE_STATUS] = STATUS_YIELDED;
}
