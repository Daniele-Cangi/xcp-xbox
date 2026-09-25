#pragma once

#include <cstdint>
#include <string>

namespace XComputeProbe
{
    inline constexpr uint32_t WorkerXvmRegisterCountValue = 16;
    inline constexpr uint32_t WorkerXvmInstructionWordsValue = 4;
    inline constexpr uint32_t WorkerXvmInputWordCountValue = 11;

    enum class WorkerXvmOpcode : uint32_t
    {
        Halt = 0,
        MoveImmediate = 1,
        LoadInputU32 = 2,
        AddU32 = 3,
        XorU32 = 4,
        MultiplyU32 = 5,
        RotateLeftU32 = 6,
        LoadMemoryU32 = 7,
        StoreMemoryU32 = 8,
        EqualU32 = 9,
        BranchIfZero = 10,
        LoopBegin = 11,
        LoopEnd = 12,
        OutputU32 = 13,
        SetControl = 14,
        Call = 15,
        Return = 16,
        IfZero = 17,
        Else = 18,
        EndIf = 19,
        LessThanU32 = 20,
        LessThanS32 = 21,
        SelectU32 = 22,
        BreakIfZero = 23,
        ContinueIfZero = 24,
        TrapIfZero = 25,
    };

    struct WorkerXvmIsaSpec
    {
        wchar_t const* isaVersion;
        wchar_t const* programSchemaVersion;
        wchar_t const* artifactKind;
        wchar_t const* profile;
        wchar_t const* computeKind;
        uint32_t highestOpcode;
        uint64_t maxCallDepth;
        bool structuredCalls;
    };

    WorkerXvmIsaSpec const& WorkerXvmV1Spec();
    WorkerXvmIsaSpec const& WorkerXvmV2Spec();
    WorkerXvmIsaSpec const* WorkerFindXvmIsaSpec(std::wstring const& isaVersion);
    bool WorkerXvmOpcodeIsAdmitted(WorkerXvmIsaSpec const& spec, WorkerXvmOpcode opcode);

    uint32_t WorkerXvmRegisterCount();
    uint32_t WorkerXvmInstructionWords();
    uint32_t WorkerXvmInputWordCount();
    uint64_t WorkerXvmInstructionLimit();
    uint64_t WorkerXvmLoopIterationLimit();
    uint64_t WorkerXvmLoopDepthLimit();
    uint64_t WorkerXvmMaxCallDepth();
    uint64_t WorkerXvmConditionalDepthLimit();
    uint64_t WorkerXvmTrapCodeLimit();
    std::wstring WorkerXvmIsaCatalogJson();
    std::wstring WorkerXvmIsaDefinitionJson();
    std::wstring WorkerXvmIsaDefinitionSha256();
    std::wstring WorkerXvmPublishedErrorCatalogJson();
    std::wstring WorkerXvmSubmissionProfileJson();
}
