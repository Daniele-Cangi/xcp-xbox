using System.Diagnostics;

namespace XComputeControlCenter.Core;

public sealed class XcpStudioWorkflowCliClient : IXcpStudioWorkflowClient
{
    private const string PairingCodeEnvironment = "XCP_PAIRING_CODE";
    private const string SessionIdEnvironment = "XCP_SESSION_ID";

    private readonly string _pythonExecutable;
    private readonly string _lifecycleToolPath;
    private readonly string _adaptationToolPath;
    private readonly string _evolutionToolPath;
    private readonly bool _inheritAuthenticationEnvironment;
    private readonly IStudioWorkerSessionProvider? _sessionProvider;

    public XcpStudioWorkflowCliClient(
        string lifecycleToolPath,
        string adaptationToolPath,
        string evolutionToolPath,
        string pythonExecutable = "python",
        bool inheritAuthenticationEnvironment = true,
        IStudioWorkerSessionProvider? sessionProvider = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(pythonExecutable);
        _lifecycleToolPath = RequireTool(
            lifecycleToolPath,
            "C5 universal lifecycle");
        _adaptationToolPath = RequireTool(
            adaptationToolPath,
            "C6 source adaptation");
        _evolutionToolPath = RequireTool(
            evolutionToolPath,
            "C7 project evolution");
        _pythonExecutable = pythonExecutable;
        _inheritAuthenticationEnvironment = inheritAuthenticationEnvironment;
        _sessionProvider = sessionProvider;
    }

    public Task<StudioCommandResult> DescribeLifecycleAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(_lifecycleToolPath, ["describe"], cancellationToken);

    public Task<StudioCommandResult> DescribeAdaptationAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(_adaptationToolPath, ["describe"], cancellationToken);

    public Task<StudioCommandResult> DescribeEvolutionAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(_evolutionToolPath, ["describe"], cancellationToken);

    public Task<StudioCommandResult> InitIntentAsync(
        StudioIntentRequest request,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _lifecycleToolPath,
            [
                "init-intent",
                "--output", FullPath(request.OutputPath),
                "--project-id", request.ProjectId,
                "--summary", request.Summary,
                "--experience-kind", request.ExperienceKind,
            ],
            cancellationToken);

    public Task<StudioCommandResult> InitLedgerAsync(
        string outputPath,
        string projectId,
        string intentPath,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _lifecycleToolPath,
            [
                "init-ledger",
                "--output", FullPath(outputPath),
                "--project-id", projectId,
                "--intent", FullPath(intentPath),
            ],
            cancellationToken);

    public Task<StudioCommandResult> DiscoverHostAsync(
        string deviceAddress,
        string outputPath,
        int port = 8787,
        double timeoutSeconds = 15,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _lifecycleToolPath,
            [
                "discover",
                "--device-address", Required(deviceAddress, "device address"),
                "--port", port.ToString(
                    System.Globalization.CultureInfo.InvariantCulture),
                "--timeout-seconds", timeoutSeconds.ToString(
                    System.Globalization.CultureInfo.InvariantCulture),
                "--output", FullPath(outputPath),
            ],
            cancellationToken,
            deviceAddress,
            port,
            timeoutSeconds);

    public Task<StudioCommandResult> RunLifecycleAsync(
        StudioLifecycleRequest request,
        CancellationToken cancellationToken = default)
    {
        var arguments = new List<string>
        {
            "run-live",
            "--device-address", Required(request.DeviceAddress, "device address"),
            "--port", request.Port.ToString(
                System.Globalization.CultureInfo.InvariantCulture),
            "--timeout-seconds", request.TimeoutSeconds.ToString(
                System.Globalization.CultureInfo.InvariantCulture),
            "--project-dir", FullPath(request.ProjectDirectory),
            "--update-project-dir", FullPath(request.UpdateProjectDirectory),
            "--bundle-root", FullPath(request.BundleRoot),
            "--intent", FullPath(request.IntentPath),
            "--capture-root", FullPath(request.CaptureRoot),
            "--receipt", FullPath(request.ReceiptPath),
            "--ledger", FullPath(request.LedgerPath),
            "--run-id", Required(request.RunId, "run id"),
        };
        if (request.ProveStructuredLiveCorrection)
        {
            arguments.Add("--prove-structured-live-correction");
        }
        if (!string.IsNullOrWhiteSpace(request.PendingCorrectionPath))
        {
            arguments.Add("--pending-correction");
            arguments.Add(FullPath(request.PendingCorrectionPath));
        }
        return ExecuteAsync(
            _lifecycleToolPath,
            arguments,
            cancellationToken,
            request.DeviceAddress,
            request.Port,
            request.TimeoutSeconds);
    }

    public Task<StudioCommandResult> RunPreviewAsync(
        StudioPreviewRequest request,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _lifecycleToolPath,
            [
                "run-preview",
                "--device-address", Required(request.DeviceAddress, "device address"),
                "--port", request.Port.ToString(
                    System.Globalization.CultureInfo.InvariantCulture),
                "--timeout-seconds", request.TimeoutSeconds.ToString(
                    System.Globalization.CultureInfo.InvariantCulture),
                "--project-dir", FullPath(request.ProjectDirectory),
                "--bundle-root", FullPath(request.BundleRoot),
                "--intent", FullPath(request.IntentPath),
                "--capture-root", FullPath(request.CaptureRoot),
                "--receipt", FullPath(request.ReceiptPath),
                "--ledger", FullPath(request.LedgerPath),
                "--run-id", Required(request.RunId, "run id"),
            ],
            cancellationToken,
            request.DeviceAddress,
            request.Port,
            request.TimeoutSeconds);

    public Task<StudioCommandResult> DetectSourceAsync(
        string sourceDirectory,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _adaptationToolPath,
            ["detect", "--source", FullPath(sourceDirectory)],
            cancellationToken);

    public Task<StudioCommandResult> AdaptSourceAsync(
        StudioAdaptationRequest request,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _adaptationToolPath,
            [
                "adapt",
                "--source", FullPath(request.SourceDirectory),
                "--output", FullPath(request.OutputDirectory),
                "--host-profile", FullPath(request.HostProfilePath),
                "--project-name", request.ProjectName,
                "--origin-kind", request.OriginKind,
                "--origin-locator", request.OriginLocator,
                "--revision", request.Revision,
                "--authorization-basis", request.AuthorizationBasis,
                "--authorization-status", request.AuthorizationStatus,
                "--license-expression", request.LicenseExpression,
                "--attribution", request.Attribution,
            ],
            cancellationToken);

    public Task<StudioCommandResult> FinalizeAdaptationAsync(
        StudioFinalizationRequest request,
        CancellationToken cancellationToken = default) =>
        FinalizeAsync(_adaptationToolPath, request, cancellationToken);

    public Task<StudioCommandResult> EvolveProjectAsync(
        StudioEvolutionRequest request,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            _evolutionToolPath,
            [
                "evolve",
                "--prior-run", FullPath(request.PriorRunDirectory),
                "--source", FullPath(request.SourceDirectory),
                "--overlay", FullPath(request.OverlayPath),
                "--output", FullPath(request.OutputDirectory),
                "--host-profile", FullPath(request.HostProfilePath),
                "--project-name", request.ProjectName,
                "--origin-kind", request.OriginKind,
                "--origin-locator", request.OriginLocator,
                "--revision", request.Revision,
                "--authorization-basis", request.AuthorizationBasis,
                "--authorization-status", request.AuthorizationStatus,
                "--license-expression", request.LicenseExpression,
                "--attribution", request.Attribution,
            ],
            cancellationToken);

    public Task<StudioCommandResult> FinalizeEvolutionAsync(
        StudioFinalizationRequest request,
        CancellationToken cancellationToken = default) =>
        FinalizeAsync(_evolutionToolPath, request, cancellationToken);

    private Task<StudioCommandResult> FinalizeAsync(
        string toolPath,
        StudioFinalizationRequest request,
        CancellationToken cancellationToken) =>
        ExecuteAsync(
            toolPath,
            [
                "finalize",
                "--run-root", FullPath(request.RunRoot),
                "--receipt", FullPath(request.ReceiptPath),
                "--ledger", FullPath(request.LedgerPath),
                "--output", FullPath(request.OutputPath),
            ],
            cancellationToken);

    private async Task<StudioCommandResult> ExecuteAsync(
        string toolPath,
        IEnumerable<string> arguments,
        CancellationToken cancellationToken,
        string? authenticationDeviceAddress = null,
        int authenticationPort = 8787,
        double authenticationTimeoutSeconds = 15)
    {
        IStudioWorkerSessionLease? session = null;
        if (!string.IsNullOrWhiteSpace(authenticationDeviceAddress) &&
            _sessionProvider is not null)
        {
            session = await _sessionProvider.OpenAsync(
                authenticationDeviceAddress,
                authenticationPort,
                authenticationTimeoutSeconds,
                cancellationToken).ConfigureAwait(false);
        }
        await using var sessionScope = session;
        var startInfo = new ProcessStartInfo
        {
            FileName = _pythonExecutable,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(toolPath)!,
        };
        StudioPythonToolLauncher.Configure(startInfo, toolPath, arguments);
        if (!_inheritAuthenticationEnvironment || session is not null)
        {
            startInfo.Environment.Remove(PairingCodeEnvironment);
            startInfo.Environment.Remove(SessionIdEnvironment);
        }
        if (session is not null)
        {
            startInfo.Environment[SessionIdEnvironment] = session.SessionId;
        }

        using var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            throw new StudioProtocolException(
                "The authoritative XCP workflow tool did not start.");
        }

        var outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var errorTask = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        var output = await outputTask.ConfigureAwait(false);
        var diagnostic = await errorTask.ConfigureAwait(false);
        return StudioCommandProtocol.Parse(
            output,
            process.ExitCode,
            diagnostic,
            $"The authoritative {Path.GetFileName(toolPath)} workflow");
    }

    private static string RequireTool(string path, string label)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath))
        {
            throw new FileNotFoundException(
                $"The authoritative {label} tool was not found.",
                fullPath);
        }
        return fullPath;
    }

    private static string FullPath(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        return Path.GetFullPath(path);
    }

    private static string Required(string value, string label)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        return value;
    }
}
