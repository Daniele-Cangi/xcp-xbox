#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace XComputeProbe
{
    struct WorkerCpuCapsuleError : std::runtime_error
    {
        WorkerCpuCapsuleError(std::string codeValue, std::string messageValue)
            : std::runtime_error(messageValue), code(std::move(codeValue)), message(std::move(messageValue))
        {
        }

        std::string code;
        std::string message;
    };

    class WorkerCpuCapsuleRuntime
    {
    public:
        WorkerCpuCapsuleRuntime();

        static wchar_t const* GateId();
        static wchar_t const* ProbeSchemaVersion();
        static wchar_t const* DependencyPackageName();
        static wchar_t const* ModuleName();
        static uint32_t AbiVersion();

        std::wstring Probe(std::wstring const& protocolVersion) const;
        std::wstring const& ActivationId() const { return activationId_; }

    private:
        std::wstring activationId_;
    };
}