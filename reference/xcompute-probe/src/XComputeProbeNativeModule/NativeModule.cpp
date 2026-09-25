#include <cstdint>
#include <windows.h>

extern "C" __declspec(dllexport) uint32_t XComputeDllAdd(uint32_t left, uint32_t right)
{
    return left + right;
}

extern "C" __declspec(dllexport) uint32_t XComputeDllMul(uint32_t left, uint32_t right)
{
    return left * right;
}

extern "C" __declspec(dllexport) uint32_t XComputeDllHash32(uint8_t const* data, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

extern "C" __declspec(dllexport) uint64_t XComputeDllVectorLoop(uint32_t const* data, size_t length, uint32_t rounds)
{
    uint64_t acc = 0;
    for (uint32_t round = 0; round < rounds; ++round)
    {
        for (size_t i = 0; i < length; ++i)
        {
            uint64_t value = data[i] + round;
            value ^= value << 7;
            value ^= value >> 3;
            acc += value;
        }
    }
    return acc;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
