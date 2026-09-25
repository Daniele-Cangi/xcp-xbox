using System.Text.Json;

namespace XComputeControlCenter.Core;

internal static class StudioCommandProtocol
{
    public static StudioCommandResult Parse(
        string output,
        int exitCode,
        string diagnostic,
        string source)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(output);
        }
        catch (JsonException exception)
        {
            throw new StudioProtocolException(
                $"{source} returned non-JSON output (exit {exitCode}): " +
                $"{exception.Message}; diagnostic bytes={diagnostic.Length}.");
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("ok", out var okElement) ||
                okElement.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            {
                throw new StudioProtocolException(
                    $"{source} result does not contain a Boolean ok field.");
            }

            var ok = okElement.GetBoolean();
            if ((exitCode == 0) != ok)
            {
                throw new StudioProtocolException(
                    $"{source} exit code {exitCode} contradicts ok={ok}.");
            }

            var schemaVersion =
                root.TryGetProperty("schema_version", out var schemaElement) &&
                schemaElement.ValueKind == JsonValueKind.String
                    ? schemaElement.GetString() ?? string.Empty
                    : string.Empty;

            return new StudioCommandResult(
                ok,
                schemaVersion,
                root.Clone(),
                ok ? null : ParseError(root, source));
        }
    }

    private static StudioError ParseError(JsonElement root, string source)
    {
        if (!root.TryGetProperty("error", out var error) ||
            error.ValueKind != JsonValueKind.Object)
        {
            throw new StudioProtocolException(
                $"The failed {source} result has no structured error object.");
        }

        var code = RequiredString(error, "code", source);
        var message = RequiredString(error, "message", source);
        StudioErrorDetails? parsedDetails = null;
        if (error.TryGetProperty("details", out var details) &&
            details.ValueKind == JsonValueKind.Object)
        {
            parsedDetails = new StudioErrorDetails(
                OptionalString(details, "schema_version"),
                OptionalString(details, "stage"),
                OptionalString(details, "field"),
                OptionalString(details, "path"),
                OptionalString(details, "expected"),
                OptionalString(details, "actual"),
                OptionalString(details, "correction"),
                details.TryGetProperty("retryable", out var retryable) &&
                retryable.ValueKind == JsonValueKind.True,
                details.Clone());
        }

        return new StudioError(code, message, parsedDetails);
    }

    private static string RequiredString(
        JsonElement value,
        string name,
        string source)
    {
        var result = OptionalString(value, name);
        if (string.IsNullOrEmpty(result))
        {
            throw new StudioProtocolException(
                $"{source} result is missing string field {name}.");
        }

        return result;
    }

    private static string OptionalString(JsonElement value, string name) =>
        value.TryGetProperty(name, out var property) &&
        property.ValueKind == JsonValueKind.String
            ? property.GetString() ?? string.Empty
            : string.Empty;
}
