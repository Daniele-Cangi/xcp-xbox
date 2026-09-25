#pragma once

#include <cstdint>
#include <string>

namespace XComputeProbe
{
    struct WorkerResultEnvelopeInput
    {
        std::wstring protocolVersion;
        std::wstring nodeId;
        std::wstring command;
        std::wstring computeKind;
        std::wstring logicalSha256;
        std::wstring computeMix64;
        std::wstring controlToken;
        std::wstring inputBindingMode;
        std::wstring boundInputSha256;
        std::wstring boundInputMix64;
        std::wstring resultArtifactId;
        std::wstring resultArtifactSha256;
        std::wstring resourceUsageJson = L"{}";
        std::wstring telemetryJson = L"{}";
        uint64_t logicalBytesRead = 0;
        bool ok = false;
        bool verified = false;
    };

    wchar_t const* WorkerResultEnvelopeSchemaVersion();
    std::wstring WorkerResultFunctionalCanonicalJson(WorkerResultEnvelopeInput const& input);
    std::wstring WorkerResultEnvelopeJson(WorkerResultEnvelopeInput const& input);
}
