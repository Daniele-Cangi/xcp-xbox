#pragma once

#include <cstdint>

inline constexpr uint32_t XComputeCpuHotKernelAbiVersionV1 = 1u;
inline constexpr uint32_t XComputeCpuHotKernelStatusOk = 0u;
inline constexpr uint32_t XComputeCpuHotKernelStatusInvalidArgument = 1u;
inline constexpr uint32_t XComputeCpuHotKernelStatusIterationLimit = 2u;
inline constexpr uint32_t XComputeCpuHotKernelLoopIterationsV1 = 65536u;
inline constexpr uint32_t XComputeCpuHotKernelMaximumChunkIterationsV1 = 4096u;
inline constexpr uint32_t XComputeCpuHotKernelAddConstantV1 = 2654435769u;
inline constexpr uint32_t XComputeCpuHotKernelXorConstantV1 = 2246822507u;
inline constexpr uint32_t XComputeCpuHotKernelMultiplyConstantV1 = 2654435769u;
inline constexpr uint32_t XComputeCpuHotKernelRotateBitsV1 = 13u;
inline constexpr uint64_t XComputeCpuHotKernelActualFuelV1 = 327694ull;

#pragma pack(push, 8)
struct XComputeCpuHotKernelProfileV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    char profileId[64];
    char profileContractSha256[65];
    char programSha256[65];
    char executionPlanSha256[65];
    uint32_t loopIterations;
    uint32_t maximumChunkIterations;
    uint32_t addConstant;
    uint32_t xorConstant;
    uint32_t multiplyConstant;
    uint32_t rotateBits;
    uint64_t actualFuel;
    uint32_t reserved[8];
};

struct XComputeCpuHotKernelRequestV1
{
    uint32_t structSize;
    uint32_t iterations;
    uint32_t initialValue;
    uint32_t reserved;
};

struct XComputeCpuHotKernelResultV1
{
    uint32_t structSize;
    uint32_t status;
    uint32_t iterationsCompleted;
    uint32_t finalValue;
    uint64_t bodySourceInstructions;
    uint32_t reserved[4];
};
#pragma pack(pop)

static_assert(sizeof(XComputeCpuHotKernelProfileV1) == 336u);
static_assert(sizeof(XComputeCpuHotKernelRequestV1) == 16u);
static_assert(sizeof(XComputeCpuHotKernelResultV1) == 40u);

using XComputeCpuCapsuleHotKernelAbiVersionFunction = uint32_t(__cdecl*)();
using XComputeCpuCapsuleQueryHotKernelV1Function = uint32_t(__cdecl*)(XComputeCpuHotKernelProfileV1*);
using XComputeCpuCapsuleRunHotKernelChunkV1Function = uint32_t(__cdecl*)(
    XComputeCpuHotKernelRequestV1 const*,
    XComputeCpuHotKernelResultV1*);
