using System.Text.Json;

namespace XComputeControlCenter.Core;

public interface IXcpCreativeLiveClient
{
    Task<StudioCommandResult> DescribeAdapterAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> DescribeCreativeHostAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> PrepareCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> CommitCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> ActivateCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> LaunchCreativeProjectAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> ObserveCreativeForegroundAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> CaptureCreativeFrameAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> RollbackCreativeActivationAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> RemoveCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default);
}
