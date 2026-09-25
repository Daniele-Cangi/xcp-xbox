#pragma once

#include <cstdint>

inline constexpr uint32_t XComputeCpuProvableWorldsAbiVersionV1 = 1u;
inline constexpr uint32_t XComputeCpuProvableWorldsStatusOk = 0u;
inline constexpr uint32_t XComputeCpuProvableWorldsStatusInvalidArgument = 1u;
inline constexpr uint32_t XComputeCpuProvableWorldsStatusLaneLimit = 2u;
inline constexpr uint32_t XComputeCpuProvableWorldsStatusRangeInvalid = 3u;
inline constexpr uint32_t XComputeCpuProvableWorldsGridWidthV1 = 1024u;
inline constexpr uint32_t XComputeCpuProvableWorldsGridHeightV1 = 1024u;
inline constexpr uint32_t XComputeCpuProvableWorldsLaneCountV1 = 1048576u;
inline constexpr uint32_t XComputeCpuProvableWorldsMaximumChunkLanesV1 = 4096u;
inline constexpr uint32_t XComputeCpuProvableWorldsXMultiplierV1 = 374761393u;
inline constexpr uint32_t XComputeCpuProvableWorldsYMultiplierV1 = 668265263u;
inline constexpr uint32_t XComputeCpuProvableWorldsRotateBitsV1 = 13u;
inline constexpr uint32_t XComputeCpuProvableWorldsFuelPerLaneV1 = 16u;
inline constexpr uint64_t XComputeCpuProvableWorldsAggregateFuelV1 = 16777216ull;

#pragma pack(push, 8)
struct XComputeCpuProvableWorldsProfileV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    char profileId[64];
    char profileContractSha256[65];
    char programSha256[65];
    char spmdExecutionPlanSha256[65];
    char gpuExecutionPlanSha256[65];
    char gpuContractSha256[65];
    char shaderSha256[65];
    uint32_t gridWidth;
    uint32_t gridHeight;
    uint32_t laneCount;
    uint32_t maximumChunkLanes;
    uint32_t fuelPerLane;
    uint64_t aggregateFuel;
    uint32_t reserved[8];
};

struct XComputeCpuProvableWorldsRequestV1
{
    uint32_t structSize;
    uint32_t startLane;
    uint32_t laneCount;
    uint32_t inputSeed;
    uint32_t* outputWords;
    uint32_t outputCapacityWords;
    uint32_t reserved[3];
};

struct XComputeCpuProvableWorldsResultV1
{
    uint32_t structSize;
    uint32_t status;
    uint32_t lanesCompleted;
    uint32_t reserved0;
    uint64_t sourceInstructions;
    uint32_t reserved[4];
};
#pragma pack(pop)

static_assert(sizeof(XComputeCpuProvableWorldsProfileV1) == 528u);
static_assert(sizeof(XComputeCpuProvableWorldsRequestV1) == 40u);
static_assert(sizeof(XComputeCpuProvableWorldsResultV1) == 40u);

using XComputeCpuCapsuleProvableWorldsAbiVersionFunction = uint32_t(__cdecl*)();
using XComputeCpuCapsuleQueryProvableWorldsV1Function = uint32_t(__cdecl*)(
    XComputeCpuProvableWorldsProfileV1*);
using XComputeCpuCapsuleRunProvableWorldsChunkV1Function = uint32_t(__cdecl*)(
    XComputeCpuProvableWorldsRequestV1 const*,
    XComputeCpuProvableWorldsResultV1*);
