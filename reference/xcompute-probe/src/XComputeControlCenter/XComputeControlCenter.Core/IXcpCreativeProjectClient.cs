namespace XComputeControlCenter.Core;

public interface IXcpCreativeProjectClient
{
    Task<StudioCommandResult> DescribeAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> CreateAsync(
        string projectDirectory,
        string projectId,
        string title,
        string? hostProfile = null,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> ValidateAsync(
        string projectDirectory,
        string? hostProfile = null,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> BuildAsync(
        string projectDirectory,
        string outputDirectory,
        string? hostProfile = null,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> VerifyAsync(
        string bundleDirectory,
        CancellationToken cancellationToken = default);
}
