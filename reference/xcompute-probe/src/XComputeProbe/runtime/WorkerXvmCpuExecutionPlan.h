#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmInterpreter.h"
#include "WorkerXvmIsa.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    inline constexpr wchar_t const* WorkerXvmCpuExecutionPlanSchemaVersion =
        L"xvm-cpu-execution-plan-v1";
    inline constexpr wchar_t const* WorkerXvmCpuCodeletCatalogId =
        L"xvm-cpu-static-codelets-v1";

    enum class WorkerXvmCpuCodeletKind : uint32_t
    {
        Instruction = 0,
        IntegerMix4 = 1,
    };

    struct WorkerXvmCpuPlanInstruction
    {
        WorkerXvmOpcode opcode = WorkerXvmOpcode::Halt;
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        WorkerXvmCpuCodeletKind codeletKind = WorkerXvmCpuCodeletKind::Instruction;
        uint32_t sourceSpan = 1;
    };

    struct WorkerXvmCpuExecutionPlan
    {
        bool admitted = false;
        std::wstring schemaVersion;
        std::wstring codeletCatalogId;
        std::wstring programId;
        std::wstring programSha256;
        std::wstring planSha256;
        uint64_t instructionCount = 0;
        uint64_t staticWorstCaseFuel = 0;
        uint64_t codeletRecordCount = 0;
        uint64_t fusedSourceInstructionCount = 0;
        uint64_t estimatedResidentBytes = 0;
        uint32_t maximumCodeletSpan = 1;
        std::vector<WorkerXvmCpuPlanInstruction> instructions;
    };

    struct WorkerXvmCpuPlanTelemetry
    {
        uint64_t sourceInstructionsExecuted = 0;
        uint64_t dispatchCount = 0;
        uint64_t instructionDispatchCount = 0;
        uint64_t codeletDispatchCount = 0;
    };

    struct WorkerXvmCpuPlanRun
    {
        WorkerXvmRunResult result;
        WorkerXvmCpuPlanTelemetry telemetry;
    };

    WorkerXvmCpuExecutionPlan WorkerBuildXvmCpuExecutionPlan(
        WorkerXvmProgram const& program,
        std::wstring const& programSha256,
        uint64_t staticWorstCaseFuel);

    std::wstring WorkerXvmCpuExecutionPlanJson(
        WorkerXvmCpuExecutionPlan const& plan);

    bool WorkerXvmCpuExecutionPlansEqual(
        WorkerXvmCpuExecutionPlan const& expected,
        WorkerXvmCpuExecutionPlan const& actual);

    WorkerXvmAdvanceStatus WorkerAdvanceXvmCpuPlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        WorkerXvmMachineState& state,
        uint64_t checkpointFuel,
        WorkerXvmCpuPlanTelemetry& telemetry,
        std::function<bool()> const& cancelRequested = {});

    WorkerXvmCpuPlanRun WorkerRunXvmCpuPlan(
        WorkerXvmProgram const& program,
        WorkerXvmCpuExecutionPlan const& plan,
        WorkerGraphNodeResourceLimits const& limits,
        std::array<uint32_t, WorkerXvmInputWordCountValue> const& inputs,
        std::function<bool()> const& cancelRequested = {});

    bool WorkerXvmMachineStatesEqual(
        WorkerXvmMachineState const& expected,
        WorkerXvmMachineState const& actual);

    bool WorkerXvmRunResultsEqual(
        WorkerXvmRunResult const& expected,
        WorkerXvmRunResult const& actual);
}
