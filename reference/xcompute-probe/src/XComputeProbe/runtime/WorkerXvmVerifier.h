#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Data.Json.h>

#include "WorkerXvmFuelAnalysis.h"
#include "WorkerXvmTypes.h"

namespace XComputeProbe
{
    std::vector<std::wstring> WorkerReadXvmCapabilities(
        winrt::Windows::Data::Json::JsonObject const& object,
        wchar_t const* field,
        bool required);

    bool WorkerXvmHasCapability(
        std::vector<std::wstring> const& capabilities,
        wchar_t const* capability);

    WorkerXvmProgram WorkerParseXvmProgram(std::string const& jsonUtf8);
    WorkerXvmStaticFuelProof WorkerVerifyXvmProgram(WorkerXvmProgram const& program);
    WorkerXvmProgram WorkerParseAndVerifyXvmProgram(std::string const& jsonUtf8);
}
