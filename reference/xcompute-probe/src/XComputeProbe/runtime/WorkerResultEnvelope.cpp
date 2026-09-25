#include "pch.h"
#include "WorkerResultEnvelope.h"
#include "WorkerCanonicalResultTelemetryRuntime.h"

namespace XComputeProbe
{
    wchar_t const* WorkerResultEnvelopeSchemaVersion()
    {
        return WorkerCanonicalResultTelemetryEnvelopeSchemaVersion();
    }

    std::wstring WorkerResultFunctionalCanonicalJson(WorkerResultEnvelopeInput const& input)
    {
        return WorkerBuildCanonicalResultTelemetryBoundary(input).functionalCanonicalJson;
    }

    std::wstring WorkerResultEnvelopeJson(WorkerResultEnvelopeInput const& input)
    {
        return WorkerBuildCanonicalResultTelemetryBoundary(input).resultEnvelopeJson;
    }
}
