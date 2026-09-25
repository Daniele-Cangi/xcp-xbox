using System.Text.Json;

namespace XComputeControlCenter.Core;

public sealed record StudioErrorDetails(
    string SchemaVersion,
    string Stage,
    string Field,
    string Path,
    string Expected,
    string Actual,
    string Correction,
    bool Retryable,
    JsonElement Raw);

public sealed record StudioError(
    string Code,
    string Message,
    StudioErrorDetails? Details);

public sealed record StudioCommandResult(
    bool Ok,
    string SchemaVersion,
    JsonElement Payload,
    StudioError? Error);

public sealed class StudioProtocolException : InvalidOperationException
{
    public StudioProtocolException(string message)
        : base(message)
    {
    }
}
