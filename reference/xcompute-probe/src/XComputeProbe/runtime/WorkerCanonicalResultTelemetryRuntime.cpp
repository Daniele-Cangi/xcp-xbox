#include "pch.h"
#include "WorkerCanonicalResultTelemetryRuntime.h"
#include "../ProbeResult.h"

namespace XComputeProbe
{
    namespace
    {
        constexpr wchar_t const* ResultEnvelopeSchemaVersionValue = L"worker-runtime-result-envelope-0.1";
        constexpr wchar_t const* FunctionalCanonicalSchemaVersionValue = L"worker-runtime-functional-canonical-0.1";

        std::wstring BoolJsonLocal(bool value)
        {
            return value ? L"true" : L"false";
        }

        std::wstring ObservedGraphNodeResultJson(WorkerGraphNodeResultSummary const& summary)
        {
            return L"{\"node_id\":" + JsonString(summary.nodeId) +
                L",\"command\":" + JsonString(summary.command) +
                L",\"kernel_id\":" + JsonString(summary.kernelId) +
                L",\"ok\":" + BoolJsonLocal(summary.ok) +
                L",\"compute_kind\":" + JsonString(summary.computeKind) +
                L",\"manifest_artifact_id\":" + JsonString(summary.manifestArtifactId) +
                L",\"result_artifact_id\":" + JsonString(summary.resultArtifactId) +
                L",\"result_sha256\":" + JsonString(summary.resultSha256) +
                L",\"logical_sha256\":" + JsonString(summary.logicalSha256) +
                L",\"logical_bytes_read\":" + std::to_wstring(summary.logicalBytesRead) +
                L",\"compute_mix64\":" + JsonString(summary.computeMix64) +
                L",\"control_token\":" + JsonString(summary.controlToken) +
                L",\"input_binding_mode\":" + JsonString(summary.inputBindingMode) +
                L",\"bound_input_sha256\":" + JsonString(summary.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(summary.boundInputMix64) +
                L",\"bound_input_applied\":" + BoolJsonLocal(summary.boundInputApplied) +
                L",\"verified\":" + BoolJsonLocal(summary.verified) +
                L",\"input_edge_count\":" + std::to_wstring(summary.inputEdgeCount) +
                L",\"input_edges\":" + summary.inputEdgesJson +
                L",\"resource_usage\":" + summary.resourceUsageJson +
                L",\"execution_details\":" + summary.executionDetailsJson +
                L",\"result_envelope\":" + summary.resultEnvelopeJson +
                L"}";
        }

        std::wstring CanonicalGraphNodeResultJson(WorkerGraphNodeResultSummary const& summary)
        {
            return L"{\"node_id\":" + JsonString(summary.nodeId) +
                L",\"command\":" + JsonString(summary.command) +
                L",\"kernel_id\":" + JsonString(summary.kernelId) +
                L",\"ok\":" + BoolJsonLocal(summary.ok) +
                L",\"compute_kind\":" + JsonString(summary.computeKind) +
                L",\"logical_sha256\":" + JsonString(summary.logicalSha256) +
                L",\"logical_bytes_read\":" + std::to_wstring(summary.logicalBytesRead) +
                L",\"compute_mix64\":" + JsonString(summary.computeMix64) +
                L",\"control_token\":" + JsonString(summary.controlToken) +
                L",\"input_binding_mode\":" + JsonString(summary.inputBindingMode) +
                L",\"bound_input_sha256\":" + JsonString(summary.boundInputSha256) +
                L",\"bound_input_mix64\":" + JsonString(summary.boundInputMix64) +
                L",\"bound_input_applied\":" + BoolJsonLocal(summary.boundInputApplied) +
                L",\"verified\":" + BoolJsonLocal(summary.verified) +
                L",\"input_edge_count\":" + std::to_wstring(summary.inputEdgeCount) +
                L",\"functional_canonical\":" + summary.functionalCanonicalJson +
                L"}";
        }
    }

    wchar_t const* WorkerCanonicalResultTelemetryEnvelopeSchemaVersion()
    {
        return ResultEnvelopeSchemaVersionValue;
    }

    wchar_t const* WorkerFunctionalCanonicalSchemaVersion()
    {
        return FunctionalCanonicalSchemaVersionValue;
    }

    WorkerCanonicalResultTelemetryOutput WorkerBuildCanonicalResultTelemetryBoundary(
        WorkerResultEnvelopeInput const& input)
    {
        WorkerCanonicalResultTelemetryOutput output;
        output.functionalCanonicalJson = std::wstring(L"{\"schema_version\":") +
            JsonString(WorkerFunctionalCanonicalSchemaVersion()) +
            L",\"node_id\":" + JsonString(input.nodeId) +
            L",\"command\":" + JsonString(input.command) +
            L",\"compute_kind\":" + JsonString(input.computeKind) +
            L",\"logical_sha256\":" + JsonString(input.logicalSha256) +
            L",\"logical_bytes_read\":" + std::to_wstring(input.logicalBytesRead) +
            L",\"compute_mix64\":" + JsonString(input.computeMix64) +
            L",\"control_token\":" + JsonString(input.controlToken) +
            L",\"input_binding_mode\":" + JsonString(input.inputBindingMode) +
            L",\"bound_input_sha256\":" + JsonString(input.boundInputSha256) +
            L",\"bound_input_mix64\":" + JsonString(input.boundInputMix64) +
            L",\"ok\":" + BoolJsonLocal(input.ok) +
            L",\"verified\":" + BoolJsonLocal(input.verified) +
            L"}";
        output.artifactIdentityJson = std::wstring(L"{\"artifact_id\":") +
            JsonString(input.resultArtifactId) +
            L",\"sha256\":" + JsonString(input.resultArtifactSha256) + L"}";
        output.telemetryJson = input.telemetryJson;
        output.evidenceMetadataJson = std::wstring(L"{\"protocol_version\":") +
            JsonString(input.protocolVersion) +
            L",\"resource_usage\":" + input.resourceUsageJson +
            L",\"arbitrary_native_or_host_code_executed\":false" +
            L",\"runtime_shader_compilation_used\":false}";
        output.resultEnvelopeJson = std::wstring(L"{\"schema_version\":") +
            JsonString(WorkerCanonicalResultTelemetryEnvelopeSchemaVersion()) +
            L",\"functional_canonical\":" + output.functionalCanonicalJson +
            L",\"artifact_identity\":" + output.artifactIdentityJson +
            L",\"telemetry\":" + output.telemetryJson +
            L",\"evidence_metadata\":" + output.evidenceMetadataJson +
            L"}";
        return output;
    }

    WorkerCanonicalResultTelemetryOutput WorkerFinalizeGraphNodeResultBoundary(
        std::wstring const& protocolVersion,
        WorkerGraphNodeResultSummary const& summary)
    {
        WorkerResultEnvelopeInput input;
        input.protocolVersion = protocolVersion;
        input.nodeId = summary.nodeId;
        input.command = summary.command;
        input.computeKind = summary.computeKind;
        input.logicalSha256 = summary.logicalSha256;
        input.computeMix64 = summary.computeMix64;
        input.controlToken = summary.controlToken;
        input.inputBindingMode = summary.inputBindingMode;
        input.boundInputSha256 = summary.boundInputSha256;
        input.boundInputMix64 = summary.boundInputMix64;
        input.resultArtifactId = summary.resultArtifactId;
        input.resultArtifactSha256 = summary.resultSha256;
        input.resourceUsageJson = summary.resourceUsageJson;
        input.logicalBytesRead = summary.logicalBytesRead;
        input.ok = summary.ok;
        input.verified = summary.verified;

        auto output = WorkerBuildCanonicalResultTelemetryBoundary(input);
        auto finalizedSummary = summary;
        finalizedSummary.functionalCanonicalJson = output.functionalCanonicalJson;
        finalizedSummary.resultEnvelopeJson = output.resultEnvelopeJson;
        output.observedNodeResultJson = ObservedGraphNodeResultJson(finalizedSummary);
        output.canonicalNodeResultJson = CanonicalGraphNodeResultJson(finalizedSummary);
        return output;
    }
}
