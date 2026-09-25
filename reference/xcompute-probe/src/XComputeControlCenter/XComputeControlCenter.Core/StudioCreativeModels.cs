using System.Text.Json;

namespace XComputeControlCenter.Core;

public sealed record StudioProjectModel(
    string ProjectId,
    string Version,
    string Title,
    string ProjectDirectory,
    string ProjectSha256);

public sealed record StudioHostProfileModel(
    string ProfileId,
    string HostApiVersion,
    string CanonicalProfileSha256,
    IReadOnlyList<string> ModuleKinds,
    IReadOnlyList<string> Capabilities,
    JsonElement Raw);

public sealed record StudioInstallModel(
    string ProjectId,
    string Version,
    string InstallId,
    string BundleSha256,
    string State);

public sealed record StudioActivationModel(
    string ProjectId,
    string ActiveInstallId,
    string RollbackTargetInstallId,
    string State);

public sealed record StudioSessionModel(
    string State,
    string AuthenticationMode,
    bool Ephemeral,
    bool Persisted);

public sealed record StudioObservationModel(
    string ProjectId,
    string InstallId,
    string ObservationSchemaVersion,
    JsonElement Raw);

public sealed record StudioEvidenceModel(
    string Kind,
    string SchemaVersion,
    string Sha256,
    long Bytes,
    string State);

public static class StudioHostProfileProjection
{
    public static StudioHostProfileModel FromWorkerResult(
        StudioCommandResult result)
    {
        if (!result.Ok ||
            !result.Payload.TryGetProperty("payload", out var sdkPayload) ||
            sdkPayload.ValueKind != JsonValueKind.Object ||
            !sdkPayload.TryGetProperty("creative_host", out var profile) ||
            profile.ValueKind != JsonValueKind.Object)
        {
            throw new StudioProtocolException(
                "The Studio host discovery result contains no creative_host object.");
        }

        var moduleKinds = ReadAdmittedIds(profile, "module_kinds");
        var capabilities = ReadAdmittedIds(profile, "capabilities");
        var profileHash =
            sdkPayload.TryGetProperty(
                "canonical_profile_sha256",
                out var hashElement) &&
            hashElement.ValueKind == JsonValueKind.String
                ? hashElement.GetString() ?? string.Empty
                : string.Empty;

        return new StudioHostProfileModel(
            RequiredString(profile, "profile_id"),
            RequiredString(profile, "host_api_version"),
            profileHash,
            moduleKinds,
            capabilities,
            profile.Clone());
    }

    private static IReadOnlyList<string> ReadAdmittedIds(
        JsonElement profile,
        string propertyName)
    {
        if (!profile.TryGetProperty(propertyName, out var values) ||
            values.ValueKind != JsonValueKind.Array)
        {
            throw new StudioProtocolException(
                $"The creative host profile has no {propertyName} array.");
        }

        var identifiers = new List<string>();
        foreach (var value in values.EnumerateArray())
        {
            if (value.ValueKind != JsonValueKind.Object ||
                !value.TryGetProperty("admitted", out var admitted) ||
                admitted.ValueKind != JsonValueKind.True)
            {
                continue;
            }

            identifiers.Add(RequiredString(value, "id"));
        }

        return identifiers;
    }

    private static string RequiredString(JsonElement value, string name)
    {
        if (!value.TryGetProperty(name, out var property) ||
            property.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(property.GetString()))
        {
            throw new StudioProtocolException(
                $"The creative host profile is missing string field {name}.");
        }

        return property.GetString()!;
    }
}
