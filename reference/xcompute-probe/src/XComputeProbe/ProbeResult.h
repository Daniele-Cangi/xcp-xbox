#pragma once

#include "pch.h"

namespace XComputeProbe
{
    struct ProbeResult
    {
        std::wstring id;
        std::wstring status;
        std::wstring evidenceClass;
        std::wstring startedUtc;
        std::wstring endedUtc;
        uint64_t durationMs = 0;
        std::map<std::wstring, std::wstring> values;
        std::vector<std::wstring> apiCalls;
        std::vector<std::wstring> errors;
        std::vector<std::wstring> notes;

        void AddString(std::wstring const& name, std::wstring const& value);
        void AddNumber(std::wstring const& name, uint64_t value);
        void AddBool(std::wstring const& name, bool value);
        void AddNull(std::wstring const& name, std::wstring const& reason);
        std::wstring ToJson() const;
    };

    std::wstring JsonEscape(std::wstring const& input);
    std::wstring JsonString(std::wstring const& input);
    std::wstring UtcNow();
    uint64_t ElapsedMs(std::chrono::steady_clock::time_point started);
}

