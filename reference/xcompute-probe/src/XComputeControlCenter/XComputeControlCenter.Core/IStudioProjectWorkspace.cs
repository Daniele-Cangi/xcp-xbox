namespace XComputeControlCenter.Core;

public interface IStudioProjectWorkspace
{
    Task<StudioProjectSnapshot> OpenAsync(
        string projectDirectory,
        CancellationToken cancellationToken = default);

    Task<StudioProjectSnapshot> SaveGuidedAsync(
        string projectDirectory,
        string expectedSha256,
        string title,
        string description,
        string version,
        CancellationToken cancellationToken = default);

    Task<StudioProjectSnapshot> SaveSourceAsync(
        string projectDirectory,
        string expectedSha256,
        string sourceText,
        CancellationToken cancellationToken = default);
}
