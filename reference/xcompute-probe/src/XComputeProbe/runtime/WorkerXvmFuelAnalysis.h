#pragma once

#include <cstdint>
#include <string>

#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerXvmStaticFuelProofSchemaVersion =
        L"xvm-static-fuel-proof-v1";

    struct WorkerXvmStaticFuelProof
    {
        uint64_t worstCaseFuel = 0;
        uint64_t declaredProgramMaxFuel = 0;
        uint64_t instructionCount = 0;
        uint64_t regionCount = 0;
    };

    WorkerXvmStaticFuelProof WorkerAnalyzeXvmStaticFuel(
        WorkerXvmProgram const& program);

    std::wstring WorkerXvmStaticFuelProofJson(
        WorkerXvmStaticFuelProof const& proof,
        uint64_t admittedNodeFuel);
}
