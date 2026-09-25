using System.Diagnostics;
using System.Text.Json;

namespace XComputeControlCenter.Core;

public sealed class XcpCreativeLiveCliClient : IXcpCreativeLiveClient
{
    private const string PairingCodeEnvironment = "XCP_PAIRING_CODE";
    private const string SessionIdEnvironment = "XCP_SESSION_ID";

    private readonly string _pythonExecutable;
    private readonly string _adapterPath;
    private readonly string _deviceAddress;
    private readonly int _port;
    private readonly double _timeoutSeconds;
    private readonly bool _inheritAuthenticationEnvironment;
    private readonly IStudioWorkerSessionProvider? _sessionProvider;

    public XcpCreativeLiveCliClient(
        string adapterPath,
        string deviceAddress,
        int port = 8787,
        double timeoutSeconds = 15,
        string pythonExecutable = "python",
        bool inheritAuthenticationEnvironment = true,
        IStudioWorkerSessionProvider? sessionProvider = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(adapterPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(deviceAddress);
        ArgumentException.ThrowIfNullOrWhiteSpace(pythonExecutable);
        if (port is < 1 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(port));
        }
        if (timeoutSeconds is <= 0 or > 300)
        {
            throw new ArgumentOutOfRangeException(nameof(timeoutSeconds));
        }

        _adapterPath = Path.GetFullPath(adapterPath);
        if (!File.Exists(_adapterPath))
        {
            throw new FileNotFoundException(
                "The authoritative XCP Studio live adapter was not found.",
                _adapterPath);
        }

        _deviceAddress = deviceAddress;
        _port = port;
        _timeoutSeconds = timeoutSeconds;
        _pythonExecutable = pythonExecutable;
        _inheritAuthenticationEnvironment = inheritAuthenticationEnvironment;
        _sessionProvider = sessionProvider;
    }

    public Task<StudioCommandResult> DescribeAdapterAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteProcessAsync(["describe"], null, cancellationToken);

    public Task<StudioCommandResult> DescribeCreativeHostAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "describe_creative_host",
            EmptyPayload(),
            cancellationToken);

    public Task<StudioCommandResult> PrepareCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "prepare_creative_install",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> CommitCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "commit_creative_install",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> ActivateCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "activate_creative_install",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> LaunchCreativeProjectAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "launch_creative_project",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> ObserveCreativeForegroundAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "observe_creative_foreground",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> CaptureCreativeFrameAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "capture_creative_frame",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> RollbackCreativeActivationAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "rollback_creative_activation",
            payload,
            cancellationToken);

    public Task<StudioCommandResult> RemoveCreativeInstallAsync(
        JsonElement payload,
        CancellationToken cancellationToken = default) =>
        ExecuteCommandAsync(
            "remove_creative_install",
            payload,
            cancellationToken);

    private Task<StudioCommandResult> ExecuteCommandAsync(
        string command,
        JsonElement payload,
        CancellationToken cancellationToken)
    {
        if (payload.ValueKind != JsonValueKind.Object)
        {
            throw new ArgumentException(
                "Studio worker command payloads must be JSON objects.",
                nameof(payload));
        }

        var request = JsonSerializer.Serialize(
            new
            {
                schema_version = "worker-sdk-request-v1",
                request_id = $"studio-{Guid.NewGuid():N}",
                command,
                payload,
            });
        var arguments = new[]
        {
            "invoke",
            "--device-address",
            _deviceAddress,
            "--port",
            _port.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "--timeout-seconds",
            _timeoutSeconds.ToString(
                System.Globalization.CultureInfo.InvariantCulture),
        };
        return ExecuteProcessAsync(arguments, request, cancellationToken);
    }

    private async Task<StudioCommandResult> ExecuteProcessAsync(
        IEnumerable<string> arguments,
        string? request,
        CancellationToken cancellationToken)
    {
        IStudioWorkerSessionLease? session = null;
        if (request is not null && _sessionProvider is not null)
        {
            session = await _sessionProvider.OpenAsync(
                _deviceAddress,
                _port,
                _timeoutSeconds,
                cancellationToken).ConfigureAwait(false);
        }
        await using var sessionScope = session;
        var startInfo = new ProcessStartInfo
        {
            FileName = _pythonExecutable,
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(_adapterPath)!,
        };
        StudioPythonToolLauncher.Configure(
            startInfo,
            _adapterPath,
            arguments);
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
                "The authoritative XCP Studio live adapter did not start.");
        }

        var outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var errorTask = process.StandardError.ReadToEndAsync(cancellationToken);
        if (request is not null)
        {
            await process.StandardInput.WriteAsync(
                request.AsMemory(),
                cancellationToken).ConfigureAwait(false);
        }
        process.StandardInput.Close();
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        var output = await outputTask.ConfigureAwait(false);
        var diagnostic = await errorTask.ConfigureAwait(false);

        return StudioCommandProtocol.Parse(
            output,
            process.ExitCode,
            diagnostic,
            "The authoritative XCP Studio live adapter");
    }

    private static JsonElement EmptyPayload() =>
        JsonSerializer.SerializeToElement(new { });
}
