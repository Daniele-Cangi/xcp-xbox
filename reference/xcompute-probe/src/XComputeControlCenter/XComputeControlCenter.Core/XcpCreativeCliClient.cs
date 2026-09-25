using System.Diagnostics;

namespace XComputeControlCenter.Core;

public sealed class XcpCreativeCliClient : IXcpCreativeProjectClient
{
    private readonly string _pythonExecutable;
    private readonly string _toolPath;

    public XcpCreativeCliClient(
        string toolPath,
        string pythonExecutable = "python")
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(toolPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(pythonExecutable);

        _toolPath = Path.GetFullPath(toolPath);
        if (!File.Exists(_toolPath))
        {
            throw new FileNotFoundException(
                "The authoritative XCP creative project CLI was not found.",
                _toolPath);
        }

        _pythonExecutable = pythonExecutable;
    }

    public Task<StudioCommandResult> DescribeAsync(
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(["describe"], cancellationToken);

    public Task<StudioCommandResult> CreateAsync(
        string projectDirectory,
        string projectId,
        string title,
        string? hostProfile = null,
        CancellationToken cancellationToken = default)
    {
        var arguments = new List<string>
        {
            "create",
            "--project-dir",
            Path.GetFullPath(projectDirectory),
            "--project-id",
            projectId,
            "--title",
            title,
        };
        AddHostProfile(arguments, hostProfile);
        return ExecuteAsync(arguments, cancellationToken);
    }

    public Task<StudioCommandResult> ValidateAsync(
        string projectDirectory,
        string? hostProfile = null,
        CancellationToken cancellationToken = default)
    {
        var arguments = new List<string>
        {
            "validate",
            "--project-dir",
            Path.GetFullPath(projectDirectory),
        };
        AddHostProfile(arguments, hostProfile);
        return ExecuteAsync(arguments, cancellationToken);
    }

    public Task<StudioCommandResult> BuildAsync(
        string projectDirectory,
        string outputDirectory,
        string? hostProfile = null,
        CancellationToken cancellationToken = default)
    {
        var arguments = new List<string>
        {
            "build",
            "--project-dir",
            Path.GetFullPath(projectDirectory),
            "--output-dir",
            Path.GetFullPath(outputDirectory),
        };
        AddHostProfile(arguments, hostProfile);
        return ExecuteAsync(arguments, cancellationToken);
    }

    public Task<StudioCommandResult> VerifyAsync(
        string bundleDirectory,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(
            [
                "verify",
                "--bundle-dir",
                Path.GetFullPath(bundleDirectory),
            ],
            cancellationToken);

    private static void AddHostProfile(
        ICollection<string> arguments,
        string? hostProfile)
    {
        if (string.IsNullOrWhiteSpace(hostProfile))
        {
            return;
        }

        arguments.Add("--host-profile");
        arguments.Add(Path.GetFullPath(hostProfile));
    }

    private async Task<StudioCommandResult> ExecuteAsync(
        IEnumerable<string> arguments,
        CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = _pythonExecutable,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(_toolPath)!,
        };
        StudioPythonToolLauncher.Configure(startInfo, _toolPath, arguments);

        using var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            throw new StudioProtocolException(
                "The authoritative XCP creative project CLI did not start.");
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
            "The authoritative XCP creative project CLI");
    }

}
