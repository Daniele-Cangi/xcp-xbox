#include <cstdint>
#include <cstring>
#include <windows.h>

#include "../shared/XComputeCpuCapsuleHotKernelV1Abi.h"
#include "../shared/XComputeCpuCapsuleProvableWorldsV1Abi.h"

namespace
{
    constexpr char HotKernelProfileId[] = "xvm_cpu_hot_kernel_mix65536_trap_v1";
    constexpr char HotKernelProfileContractSha256[] = "fc911fe4581e08112bb47a7cd8852185fc4d4d22f716a1516acd17ee48eead11";
    constexpr char HotKernelProgramSha256[] = "6e4fc53399a76ccb17fc98f280c186fce735582554630a2ebe3e2f316a870370";
    constexpr char HotKernelExecutionPlanSha256[] = "80887336b9f965f7d0f058b16b5387cc1f6937a3fad9c8c6a42ec885e4de1867";
    constexpr char ProvableWorldsProfileId[] = "provable_worlds_cpu_capsule_v1";
    constexpr char ProvableWorldsProfileContractSha256[] = "e0b6a59c07b7225135efb5fce4b0f72f18ddb903325fdbdf35b038415f366486";
    constexpr char ProvableWorldsProgramSha256[] = "5c8c06aa8700f0dcec90263c5a4243cd4ecfebbaaec25e870074e1906e8fc836";
    constexpr char ProvableWorldsSpmdPlanSha256[] = "ecb2d61bbff32fd339dc2034091f455faeed39142d377fe5de686761f9227bc1";
    constexpr char ProvableWorldsGpuPlanSha256[] = "399c105dbea185cb0ad6c4c49859ffbaff2cba089eda383063024f059770594a";
    constexpr char ProvableWorldsGpuContractSha256[] = "8587a2fc3a26782861cce2c621cbccaafe78ff249f502da9c4759ebb9d9d89b6";
    constexpr char ProvableWorldsShaderSha256[] = "835a745b098711b20c5fbf9d6b32f55ff68623e786e2854b052f86de6e08579d";

    uint64_t Transform(uint64_t seed, uint32_t rounds)
    {
        uint64_t value = seed ^ 0x9E3779B97F4A7C15ull;
        for (uint32_t round = 0; round < rounds; ++round)
        {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
            value += 0xD1B54A32D192ED03ull + static_cast<uint64_t>(round);
        }
        return value;
    }

    uint32_t RotateLeft(uint32_t value, uint32_t shift)
    {
        return (value << shift) | (value >> (32u - shift));
    }

    template <size_t Size>
    void CopyAscii(char (&destination)[Size], char const* source)
    {
        auto length = std::strlen(source);
        if (length >= Size)
        {
            length = Size - 1;
        }
        std::memcpy(destination, source, length);
        destination[length] = '\0';
    }
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleAbiVersion()
{
    return 1u;
}

extern "C" __declspec(dllexport) uint64_t XComputeCpuCapsuleBuildVersionPacked()
{
    return
        (static_cast<uint64_t>(XCOMPUTE_CPU_CAPSULE_VERSION_MAJOR) << 48) |
        (static_cast<uint64_t>(XCOMPUTE_CPU_CAPSULE_VERSION_MINOR) << 32) |
        (static_cast<uint64_t>(XCOMPUTE_CPU_CAPSULE_VERSION_BUILD) << 16) |
        static_cast<uint64_t>(XCOMPUTE_CPU_CAPSULE_VERSION_REVISION);
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleSelfTest()
{
    return 0x43504331u;
}

extern "C" __declspec(dllexport) uint64_t XComputeCpuCapsuleTransform(uint64_t seed, uint32_t rounds)
{
    return Transform(seed, rounds);
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleHotKernelAbiVersion()
{
    return XComputeCpuHotKernelAbiVersionV1;
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleQueryHotKernelV1(
    XComputeCpuHotKernelProfileV1* profile)
{
    if (profile == nullptr || profile->structSize != sizeof(XComputeCpuHotKernelProfileV1))
    {
        return XComputeCpuHotKernelStatusInvalidArgument;
    }
    XComputeCpuHotKernelProfileV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = XComputeCpuHotKernelAbiVersionV1;
    CopyAscii(value.profileId, HotKernelProfileId);
    CopyAscii(value.profileContractSha256, HotKernelProfileContractSha256);
    CopyAscii(value.programSha256, HotKernelProgramSha256);
    CopyAscii(value.executionPlanSha256, HotKernelExecutionPlanSha256);
    value.loopIterations = XComputeCpuHotKernelLoopIterationsV1;
    value.maximumChunkIterations = XComputeCpuHotKernelMaximumChunkIterationsV1;
    value.addConstant = XComputeCpuHotKernelAddConstantV1;
    value.xorConstant = XComputeCpuHotKernelXorConstantV1;
    value.multiplyConstant = XComputeCpuHotKernelMultiplyConstantV1;
    value.rotateBits = XComputeCpuHotKernelRotateBitsV1;
    value.actualFuel = XComputeCpuHotKernelActualFuelV1;
    *profile = value;
    return XComputeCpuHotKernelStatusOk;
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleRunHotKernelChunkV1(
    XComputeCpuHotKernelRequestV1 const* request,
    XComputeCpuHotKernelResultV1* result)
{
    if (request == nullptr || result == nullptr ||
        request->structSize != sizeof(XComputeCpuHotKernelRequestV1) ||
        result->structSize != sizeof(XComputeCpuHotKernelResultV1) ||
        request->reserved != 0)
    {
        return XComputeCpuHotKernelStatusInvalidArgument;
    }
    if (request->iterations > XComputeCpuHotKernelMaximumChunkIterationsV1)
    {
        return XComputeCpuHotKernelStatusIterationLimit;
    }
    uint32_t value = request->initialValue;
    for (uint32_t iteration = 0; iteration < request->iterations; ++iteration)
    {
        value += XComputeCpuHotKernelAddConstantV1;
        value ^= XComputeCpuHotKernelXorConstantV1;
        value *= XComputeCpuHotKernelMultiplyConstantV1;
        value = RotateLeft(value, XComputeCpuHotKernelRotateBitsV1);
    }
    XComputeCpuHotKernelResultV1 output{};
    output.structSize = sizeof(output);
    output.status = XComputeCpuHotKernelStatusOk;
    output.iterationsCompleted = request->iterations;
    output.finalValue = value;
    output.bodySourceInstructions = static_cast<uint64_t>(request->iterations) * 4ull;
    *result = output;
    return XComputeCpuHotKernelStatusOk;
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleProvableWorldsAbiVersion()
{
    return XComputeCpuProvableWorldsAbiVersionV1;
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleQueryProvableWorldsV1(
    XComputeCpuProvableWorldsProfileV1* profile)
{
    if (profile == nullptr || profile->structSize != sizeof(XComputeCpuProvableWorldsProfileV1))
    {
        return XComputeCpuProvableWorldsStatusInvalidArgument;
    }
    XComputeCpuProvableWorldsProfileV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = XComputeCpuProvableWorldsAbiVersionV1;
    CopyAscii(value.profileId, ProvableWorldsProfileId);
    CopyAscii(value.profileContractSha256, ProvableWorldsProfileContractSha256);
    CopyAscii(value.programSha256, ProvableWorldsProgramSha256);
    CopyAscii(value.spmdExecutionPlanSha256, ProvableWorldsSpmdPlanSha256);
    CopyAscii(value.gpuExecutionPlanSha256, ProvableWorldsGpuPlanSha256);
    CopyAscii(value.gpuContractSha256, ProvableWorldsGpuContractSha256);
    CopyAscii(value.shaderSha256, ProvableWorldsShaderSha256);
    value.gridWidth = XComputeCpuProvableWorldsGridWidthV1;
    value.gridHeight = XComputeCpuProvableWorldsGridHeightV1;
    value.laneCount = XComputeCpuProvableWorldsLaneCountV1;
    value.maximumChunkLanes = XComputeCpuProvableWorldsMaximumChunkLanesV1;
    value.fuelPerLane = XComputeCpuProvableWorldsFuelPerLaneV1;
    value.aggregateFuel = XComputeCpuProvableWorldsAggregateFuelV1;
    *profile = value;
    return XComputeCpuProvableWorldsStatusOk;
}

extern "C" __declspec(dllexport) uint32_t XComputeCpuCapsuleRunProvableWorldsChunkV1(
    XComputeCpuProvableWorldsRequestV1 const* request,
    XComputeCpuProvableWorldsResultV1* result)
{
    if (request == nullptr || result == nullptr ||
        request->structSize != sizeof(XComputeCpuProvableWorldsRequestV1) ||
        result->structSize != sizeof(XComputeCpuProvableWorldsResultV1) ||
        request->outputWords == nullptr ||
        request->reserved[0] != 0 || request->reserved[1] != 0 || request->reserved[2] != 0)
    {
        return XComputeCpuProvableWorldsStatusInvalidArgument;
    }
    if (request->laneCount > XComputeCpuProvableWorldsMaximumChunkLanesV1 ||
        request->outputCapacityWords < request->laneCount)
    {
        return XComputeCpuProvableWorldsStatusLaneLimit;
    }
    if (request->startLane > XComputeCpuProvableWorldsLaneCountV1 ||
        request->laneCount > XComputeCpuProvableWorldsLaneCountV1 - request->startLane)
    {
        return XComputeCpuProvableWorldsStatusRangeInvalid;
    }

    for (uint32_t offset = 0; offset < request->laneCount; ++offset)
    {
        auto lane = request->startLane + offset;
        auto x = lane % XComputeCpuProvableWorldsGridWidthV1;
        auto y = lane / XComputeCpuProvableWorldsGridWidthV1;
        auto combined =
            (x * XComputeCpuProvableWorldsXMultiplierV1) ^
            (y * XComputeCpuProvableWorldsYMultiplierV1);
        auto seeded = combined ^ request->inputSeed;
        auto rotated = RotateLeft(seeded, XComputeCpuProvableWorldsRotateBitsV1);
        request->outputWords[offset] = (rotated + x) ^ y;
    }

    XComputeCpuProvableWorldsResultV1 output{};
    output.structSize = sizeof(output);
    output.status = XComputeCpuProvableWorldsStatusOk;
    output.lanesCompleted = request->laneCount;
    output.sourceInstructions =
        static_cast<uint64_t>(request->laneCount) * XComputeCpuProvableWorldsFuelPerLaneV1;
    *result = output;
    return XComputeCpuProvableWorldsStatusOk;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
