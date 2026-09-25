#pragma once

#include "WorkerGraphRuntime.h"
#include "WorkerResultEnvelope.h"

#include <string>

namespace XComputeProbe
{
    struct WorkerCanonicalResultTelemetryOutput
    {
        std::wstring functionalCanonicalJson;
        std::wstring artifactIdentityJson;
        std::wstring telemetryJson;
        std::wstring evidenceMetadataJson;
        std::wstring resultEnvelopeJson;
        std::wstring observedNodeResultJson;
        std::wstring canonicalNodeResultJson;
    };

    wchar_t const* WorkerCanonicalResultTelemetryEnvelopeSchemaVersion();
    wchar_t const* WorkerFunctionalCanonicalSchemaVersion();
    WorkerCanonicalResultTelemetryOutput WorkerBuildCanonicalResultTelemetryBoundary(
        WorkerResultEnvelopeInput const& input);
    WorkerCanonicalResultTelemetryOutput WorkerFinalizeGraphNodeResultBoundary(
        std::wstring const& protocolVersion,
        WorkerGraphNodeResultSummary const& summary);
}
