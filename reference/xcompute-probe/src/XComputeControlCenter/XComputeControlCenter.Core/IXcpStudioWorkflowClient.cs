namespace XComputeControlCenter.Core;

public interface IXcpStudioWorkflowClient
{
    Task<StudioCommandResult> DescribeLifecycleAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> DescribeAdaptationAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> DescribeEvolutionAsync(
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> InitIntentAsync(
        StudioIntentRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> InitLedgerAsync(
        string outputPath,
        string projectId,
        string intentPath,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> DiscoverHostAsync(
        string deviceAddress,
        string outputPath,
        int port = 8787,
        double timeoutSeconds = 15,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> RunLifecycleAsync(
        StudioLifecycleRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> RunPreviewAsync(
        StudioPreviewRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> DetectSourceAsync(
        string sourceDirectory,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> AdaptSourceAsync(
        StudioAdaptationRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> FinalizeAdaptationAsync(
        StudioFinalizationRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> EvolveProjectAsync(
        StudioEvolutionRequest request,
        CancellationToken cancellationToken = default);

    Task<StudioCommandResult> FinalizeEvolutionAsync(
        StudioFinalizationRequest request,
        CancellationToken cancellationToken = default);
}
