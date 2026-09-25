using System.Security.Cryptography;
using System.Text.Json;

namespace XComputeControlCenter.Core;

public sealed record StudioPythonRuntime(
    string Root,
    string RuntimeId,
    string RuntimeVersion,
    string Architecture,
    string Executable,
    string ManifestSha256);

public sealed class StudioPythonRuntimeException : InvalidOperationException
{
    public StudioPythonRuntimeException(
        string code,
        string message,
        string path,
        string expected,
        string actual,
        string correction)
        : base(message)
    {
        Code = code;
        Details = new StudioPythonRuntimeErrorDetails(
            "xcp-studio-python-runtime-error-details-v1",
            "python_runtime_resolution",
            path,
            expected,
            actual,
            correction);
    }

    public string Code { get; }

    public StudioPythonRuntimeErrorDetails Details { get; }
}

public sealed record StudioPythonRuntimeErrorDetails(
    string SchemaVersion,
    string Stage,
    string Path,
    string Expected,
    string Actual,
    string Correction);

public static class StudioPythonRuntimeResolver
{
    public const string EnvironmentVariable = "XCP_STUDIO_PYTHON_ROOT";
    public const string ManifestFileName =
        "xcp-studio-python-runtime-manifest.json";
    public const string ExpectedRuntimeId = "xcp.studio.python";
    public const string ExpectedArchitecture = "x64";

    public static StudioPythonRuntime Resolve(
        string applicationBaseDirectory,
        string? configuredRoot = null,
        string? localApplicationData = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(applicationBaseDirectory);
        configuredRoot ??= Environment.GetEnvironmentVariable(
            EnvironmentVariable);

        if (!string.IsNullOrWhiteSpace(configuredRoot))
        {
            return Load(configuredRoot);
        }

        localApplicationData ??= Environment.GetFolderPath(
            Environment.SpecialFolder.LocalApplicationData);
        var candidates = new[]
        {
            Path.Combine(applicationBaseDirectory, "runtime", "python"),
            Path.Combine(
                localApplicationData,
                "XCP",
                "Studio",
                "runtime",
                "python"),
        };
        foreach (var candidate in candidates)
        {
            if (File.Exists(Path.Combine(candidate, ManifestFileName)))
            {
                return Load(candidate);
            }
        }

        throw Invalid(
            "xcp.studio.python_runtime_not_found",
            applicationBaseDirectory,
            $"{EnvironmentVariable}, application runtime, or per-user runtime",
            "none",
            "Install an exact XCP Studio Python runtime beside the application.");
    }

    public static StudioPythonRuntime Load(string root)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        var canonicalRoot = Path.GetFullPath(root);
        var manifestPath = Path.Combine(canonicalRoot, ManifestFileName);
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(
                File.ReadAllBytes(manifestPath),
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                });
        }
        catch (Exception exception)
            when (exception is IOException or UnauthorizedAccessException or
                  JsonException)
        {
            throw Invalid(
                "xcp.studio.python_runtime_manifest_invalid",
                manifestPath,
                "strict UTF-8 JSON xcp-studio-python-runtime-manifest-v1",
                exception.GetType().Name,
                "Build a fresh exact Studio Python runtime.");
        }

        using (document)
        {
            var manifest = document.RootElement;
            RequireString(
                manifest,
                "schema_version",
                "xcp-studio-python-runtime-manifest-v1",
                manifestPath);
            var runtimeId = RequireString(manifest, "runtime_id", manifestPath);
            if (!string.Equals(
                    runtimeId,
                    ExpectedRuntimeId,
                    StringComparison.Ordinal))
            {
                throw Invalid(
                    "xcp.studio.python_runtime_identity_invalid",
                    manifestPath,
                    ExpectedRuntimeId,
                    runtimeId,
                    "Select the canonical XCP Studio Python runtime.");
            }

            var runtimeVersion = RequireString(
                manifest,
                "runtime_version",
                manifestPath);
            if (!Version.TryParse(runtimeVersion, out var parsedVersion) ||
                parsedVersion.Major < 3 ||
                (parsedVersion.Major == 3 && parsedVersion.Minor < 11))
            {
                throw Invalid(
                    "xcp.studio.python_runtime_version_invalid",
                    manifestPath,
                    "Python >= 3.11 numeric version",
                    runtimeVersion,
                    "Build the pinned XCP Studio Python runtime.");
            }

            var architecture = RequireString(
                manifest,
                "architecture",
                manifestPath);
            if (!string.Equals(
                    architecture,
                    ExpectedArchitecture,
                    StringComparison.Ordinal))
            {
                throw Invalid(
                    "xcp.studio.python_runtime_architecture_invalid",
                    manifestPath,
                    ExpectedArchitecture,
                    architecture,
                    "Install the win-x64 Studio distribution.");
            }

            ValidateSecurity(manifest, manifestPath);
            var executableRelative = RequireString(
                manifest,
                "executable",
                manifestPath);
            var executable = ResolveContainedPath(
                canonicalRoot,
                executableRelative,
                manifestPath);
            ValidateFiles(manifest, canonicalRoot, manifestPath);
            if (!File.Exists(executable) ||
                (File.GetAttributes(executable) & FileAttributes.ReparsePoint) != 0)
            {
                throw Invalid(
                    "xcp.studio.python_runtime_executable_invalid",
                    executable,
                    "regular manifest-bound python.exe",
                    File.Exists(executable) ? "reparse point" : "missing",
                    "Reject the runtime and build exact bytes again.");
            }

            return new StudioPythonRuntime(
                canonicalRoot,
                runtimeId,
                runtimeVersion,
                architecture,
                executable,
                Convert.ToHexString(
                    SHA256.HashData(File.ReadAllBytes(manifestPath)))
                    .ToLowerInvariant());
        }
    }

    private static void ValidateSecurity(
        JsonElement manifest,
        string manifestPath)
    {
        if (!manifest.TryGetProperty("security", out var security) ||
            security.ValueKind != JsonValueKind.Object)
        {
            throw Invalid(
                "xcp.studio.python_runtime_security_invalid",
                manifestPath,
                "explicit fail-closed security object",
                security.ValueKind.ToString(),
                "Build a canonical Studio Python runtime.");
        }

        foreach (var field in new[]
                 {
                     "credentials_included",
                     "session_material_persisted",
                     "user_site_enabled",
                 })
        {
            if (!security.TryGetProperty(field, out var value) ||
                value.ValueKind is not JsonValueKind.False)
            {
                throw Invalid(
                    "xcp.studio.python_runtime_security_invalid",
                    manifestPath,
                    $"{field}=false",
                    value.ValueKind.ToString(),
                    "Reject the runtime and rebuild it from pinned inputs.");
            }
        }
    }

    private static void ValidateFiles(
        JsonElement manifest,
        string root,
        string manifestPath)
    {
        if (!manifest.TryGetProperty("files", out var files) ||
            files.ValueKind != JsonValueKind.Array)
        {
            throw Invalid(
                "xcp.studio.python_runtime_files_invalid",
                manifestPath,
                "files array",
                files.ValueKind.ToString(),
                "Build a complete canonical runtime.");
        }

        var expected = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in files.EnumerateArray())
        {
            var relative = RequireString(entry, "path", manifestPath);
            if (!expected.Add(relative))
            {
                throw Invalid(
                    "xcp.studio.python_runtime_files_invalid",
                    manifestPath,
                    "unique file paths",
                    relative,
                    "Reject the duplicate-path manifest.");
            }
            var path = ResolveContainedPath(root, relative, manifestPath);
            if (!File.Exists(path) ||
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw Invalid(
                    "xcp.studio.python_runtime_file_identity_mismatch",
                    path,
                    "regular manifest-bound file",
                    File.Exists(path) ? "reparse point" : "missing",
                    "Reject the runtime and build exact bytes again.");
            }
            if (!entry.TryGetProperty("bytes", out var bytes) ||
                !bytes.TryGetInt64(out var expectedBytes) ||
                expectedBytes < 0)
            {
                throw Invalid(
                    "xcp.studio.python_runtime_files_invalid",
                    manifestPath,
                    "non-negative byte length",
                    "invalid",
                    "Build a canonical runtime.");
            }
            var expectedHash = RequireString(entry, "sha256", manifestPath);
            var actualBytes = new FileInfo(path).Length;
            var actualHash = Convert.ToHexString(
                SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
            if (actualBytes != expectedBytes ||
                !string.Equals(actualHash, expectedHash, StringComparison.Ordinal))
            {
                throw Invalid(
                    "xcp.studio.python_runtime_file_identity_mismatch",
                    path,
                    $"{expectedBytes} bytes / {expectedHash}",
                    $"{actualBytes} bytes / {actualHash}",
                    "Reject the modified runtime.");
            }
        }

        var actual = Directory
            .EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .Where(path => !string.Equals(
                Path.GetFullPath(path),
                Path.GetFullPath(manifestPath),
                StringComparison.OrdinalIgnoreCase))
            .Select(path => Path.GetRelativePath(root, path).Replace('\\', '/'))
            .ToHashSet(StringComparer.Ordinal);
        if (!actual.SetEquals(expected))
        {
            throw Invalid(
                "xcp.studio.python_runtime_file_set_mismatch",
                root,
                string.Join(",", expected.Order()),
                string.Join(",", actual.Order()),
                "Reject the modified runtime.");
        }
    }

    private static string ResolveContainedPath(
        string root,
        string relative,
        string manifestPath)
    {
        if (Path.IsPathRooted(relative))
        {
            throw Invalid(
                "xcp.studio.python_runtime_path_invalid",
                manifestPath,
                "relative contained path",
                relative,
                "Reject the escaping manifest.");
        }
        var fullPath = Path.GetFullPath(
            Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        var prefix = root.EndsWith(Path.DirectorySeparatorChar)
            ? root
            : root + Path.DirectorySeparatorChar;
        if (!fullPath.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            throw Invalid(
                "xcp.studio.python_runtime_path_invalid",
                manifestPath,
                "relative contained path",
                relative,
                "Reject the escaping manifest.");
        }
        return fullPath;
    }

    private static string RequireString(
        JsonElement parent,
        string name,
        string path)
    {
        if (!parent.TryGetProperty(name, out var value) ||
            value.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(value.GetString()))
        {
            throw Invalid(
                "xcp.studio.python_runtime_manifest_invalid",
                path,
                $"non-empty string {name}",
                value.ValueKind.ToString(),
                "Build a fresh exact Studio Python runtime.");
        }
        return value.GetString()!;
    }

    private static void RequireString(
        JsonElement parent,
        string name,
        string expected,
        string path)
    {
        var actual = RequireString(parent, name, path);
        if (!string.Equals(actual, expected, StringComparison.Ordinal))
        {
            throw Invalid(
                "xcp.studio.python_runtime_manifest_invalid",
                path,
                expected,
                actual,
                "Build a fresh exact Studio Python runtime.");
        }
    }

    private static StudioPythonRuntimeException Invalid(
        string code,
        string path,
        string expected,
        string actual,
        string correction) =>
        new(
            code,
            "The private XCP Studio Python runtime failed closed.",
            path,
            expected,
            actual,
            correction);
}
