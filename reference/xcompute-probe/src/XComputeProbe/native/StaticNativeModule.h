#pragma once

#include "pch.h"

namespace XComputeProbe::NativeStatic
{
    uint32_t Add(uint32_t left, uint32_t right);
    uint32_t Mul(uint32_t left, uint32_t right);
    uint32_t Hash32(uint8_t const* data, size_t length);
    uint64_t VectorLoop(uint32_t const* data, size_t length, uint32_t rounds);
}
