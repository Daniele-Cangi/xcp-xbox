namespace XComputeControlCenter.Core;

public sealed record StudioProjectSnapshot(
    string ProjectDirectory,
    string ProjectPath,
    string ProjectId,
    string Version,
    string Title,
    string Description,
    string SourceText,
    string FileSha256);

public sealed record StudioIntentRequest(
    string OutputPath,
    string ProjectId,
    string Summary,
    string ExperienceKind);

public sealed record StudioLifecycleRequest(
    string DeviceAddress,
    string ProjectDirectory,
    string UpdateProjectDirectory,
    string BundleRoot,
    string IntentPath,
    string CaptureRoot,
    string ReceiptPath,
    string LedgerPath,
    string RunId,
    int Port = 8787,
    double TimeoutSeconds = 15,
    bool ProveStructuredLiveCorrection = false,
    string? PendingCorrectionPath = null);

public sealed record StudioPreviewRequest(
    string DeviceAddress,
    string ProjectDirectory,
    string BundleRoot,
    string IntentPath,
    string CaptureRoot,
    string ReceiptPath,
    string LedgerPath,
    string RunId,
    int Port = 8787,
    double TimeoutSeconds = 15);

public sealed record StudioAdaptationRequest(
    string SourceDirectory,
    string OutputDirectory,
    string HostProfilePath,
    string ProjectName,
    string OriginKind,
    string OriginLocator,
    string Revision,
    string AuthorizationBasis,
    string AuthorizationStatus,
    string Attribution,
    string LicenseExpression = "");

public sealed record StudioEvolutionRequest(
    string PriorRunDirectory,
    string SourceDirectory,
    string OverlayPath,
    string OutputDirectory,
    string HostProfilePath,
    string ProjectName,
    string OriginKind,
    string OriginLocator,
    string Revision,
    string AuthorizationBasis,
    string AuthorizationStatus,
    string Attribution,
    string LicenseExpression = "");

public sealed record StudioFinalizationRequest(
    string RunRoot,
    string ReceiptPath,
    string LedgerPath,
    string OutputPath);

public sealed record StudioWorkspaceErrorDetails(
    string Stage,
    string Path,
    string Expected,
    string Actual,
    string Correction);

public sealed class StudioWorkspaceException : InvalidOperationException
{
    public StudioWorkspaceException(
        string code,
        string message,
        StudioWorkspaceErrorDetails details,
        Exception? innerException = null)
        : base(message, innerException)
    {
        Code = code;
        Details = details;
    }

    public string Code { get; }

    public StudioWorkspaceErrorDetails Details { get; }
}
