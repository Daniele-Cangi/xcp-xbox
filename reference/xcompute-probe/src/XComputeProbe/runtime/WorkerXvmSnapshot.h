#pragma once

#include <string>

#include "WorkerGraphResourceLedger.h"
#include "WorkerXvmSnapshotAuthority.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    struct WorkerXvmSnapshotContext
    {
        std::wstring isaVersion;
        std::wstring programId;
        std::wstring programSha256;
        std::wstring boundInputSha256;
        std::wstring executionPlanSha256;
        std::wstring sourceBackend;
        std::wstring profileContractSha256;
        std::wstring moduleSha256;
        bool structuredControlPhaseB = false;
        bool tieredCpuState = false;
        bool packagedCapsuleCpuState = false;
    };

    struct WorkerXvmSnapshotProvenance
    {
        std::wstring executionId;
        uint64_t checkpointSequence = 0;
        std::wstring previousStateSha256;
    };

    struct WorkerXvmSnapshotResumeAuthorization
    {
        std::wstring expectedExecutionId;
        uint64_t expectedCheckpointSequence = 0;
        std::wstring expectedPreviousStateSha256;
    };

    struct WorkerXvmRestoredSnapshot
    {
        WorkerXvmMachineState state;
        WorkerXvmSnapshotProvenance provenance;
        std::wstring workerSealSha256;
    };

    wchar_t const* WorkerXvmStateSnapshotSchemaVersion();
    wchar_t const* WorkerXvmLatestStateSnapshotSchemaVersion();
    wchar_t const* WorkerXvmStateSnapshotSchemaVersionForProgram(WorkerXvmProgram const& program);
    wchar_t const* WorkerXvmStateSnapshotSchemaVersionForExecution(
        WorkerXvmProgram const& program,
        bool tieredCpuState,
        bool packagedCapsuleCpuState = false);
    wchar_t const* WorkerXvmStateDigestSchemaVersion();

    std::wstring WorkerXvmMachineStateCanonicalJson(
        WorkerXvmSnapshotContext const& context,
        WorkerXvmMachineState const& state);

    std::wstring WorkerXvmStateSnapshotJson(
        WorkerXvmSnapshotContext const& context,
        WorkerXvmSnapshotProvenance const& provenance,
        WorkerXvmMachineState const& state,
        WorkerXvmSnapshotAuthority const& authority);

    WorkerXvmRestoredSnapshot WorkerRestoreXvmStateSnapshot(
        std::wstring const& snapshotJson,
        WorkerXvmSnapshotContext const& expectedContext,
        WorkerXvmSnapshotResumeAuthorization const* authorization,
        WorkerXvmProgram const& program,
        WorkerGraphNodeResourceLimits const& limits,
        WorkerXvmSnapshotAuthority const& authority);
}