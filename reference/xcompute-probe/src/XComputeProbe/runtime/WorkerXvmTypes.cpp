#include "pch.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    WorkerXvmError::WorkerXvmError(std::string codeValue, std::string messageValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue))
    {
    }

    WorkerXvmError::WorkerXvmError(
        std::string codeValue,
        std::string messageValue,
        WorkerXvmErrorDetails detailsValue) :
        code(std::move(codeValue)),
        message(std::move(messageValue)),
        details(std::move(detailsValue))
    {
    }

    char const* WorkerXvmError::what() const noexcept
    {
        return message.c_str();
    }
}
