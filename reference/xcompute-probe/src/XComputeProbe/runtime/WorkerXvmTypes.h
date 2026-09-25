#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace XComputeProbe
{
    struct WorkerXvmErrorDetails
    {
        std::string stage = "worker";
        std::string field;
        std::string expected;
        std::string actual;
        std::string correction =
            "branch on error.code, refresh the published XVM contracts, and fail closed";
        bool retryable = false;
    };

    struct WorkerXvmError : std::exception
    {
        std::string code;
        std::string message;
        WorkerXvmErrorDetails details;

        WorkerXvmError(std::string codeValue, std::string messageValue);
        WorkerXvmError(
            std::string codeValue,
            std::string messageValue,
            WorkerXvmErrorDetails detailsValue);
        char const* what() const noexcept override;
    };

    struct WorkerXvmFunction
    {
        uint32_t entryPc = 0;
        uint32_t endPc = 0;
    };

    enum class WorkerXvmViewAccess
    {
        Read,
        Write,
        ReadWrite,
    };

    struct WorkerXvmTypedView
    {
        std::wstring viewId;
        std::wstring elementType;
        WorkerXvmViewAccess access = WorkerXvmViewAccess::Read;
        uint64_t offsetBytes = 0;
        uint64_t lengthBytes = 0;
    };

    struct WorkerXvmProgram
    {
        std::wstring schemaVersion;
        std::wstring isaVersion;
        std::wstring artifactKind;
        std::wstring profile;
        std::wstring computeKind;
        std::wstring programId;
        std::vector<std::wstring> capabilities;
        std::vector<uint32_t> words;
        std::vector<WorkerXvmFunction> functions;
        std::vector<WorkerXvmTypedView> inputViews;
        std::vector<WorkerXvmTypedView> memoryViews;
        std::vector<WorkerXvmTypedView> outputViews;
        uint64_t memoryBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t maxFuel = 0;
        uint64_t maxCallDepth = 0;
        uint64_t staticWorstCaseFuel = 0;
    };

    struct WorkerXvmTrapState
    {
        uint32_t code = 0;
        uint32_t trapPc = 0;
        uint32_t recoveryPc = 0;
        uint64_t occurrenceCount = 0;
    };

    struct WorkerXvmRunResult
    {
        std::vector<uint8_t> output;
        std::wstring controlToken = L"fail";
        uint64_t fuelConsumed = 0;
        WorkerXvmTrapState trap;
    };

    struct WorkerXvmLoopFrame
    {
        uint32_t beginPc = 0;
        uint32_t endPc = 0;
        uint32_t remaining = 0;
    };

    struct WorkerXvmCallFrame
    {
        uint32_t returnPc = 0;
        uint32_t loopDepth = 0;
    };

    struct WorkerXvmMachineState
    {
        std::vector<uint32_t> registers;
        std::vector<uint8_t> memory;
        std::vector<uint8_t> output;
        std::vector<WorkerXvmLoopFrame> loopStack;
        std::vector<WorkerXvmCallFrame> callStack;
        std::wstring controlToken = L"fail";
        uint64_t fuelConsumed = 0;
        uint32_t pc = 0;
        WorkerXvmTrapState trap;
        bool halted = false;
    };
}
