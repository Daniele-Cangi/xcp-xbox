using System.Security.Cryptography;
using System.Text.Json;

namespace XComputeControlCenter.Core;

public sealed record StudioToolchain(
    string Root,
    string KitId,
    string KitVersion,
    string ManifestSha256,
    IReadOnlyDictionary<string, string> Entrypoints)
{
    public string Entrypoint(string name) =>
        Entrypoints.TryGetValue(name, out var path)
            ? path
            : throw new StudioToolchainException(
                "xcp.studio.toolchain_entrypoint_missing",
                $"The resolved toolchain does not publish '{name}'.",
                Root,
                "published manifest entrypoint",
                name,
                "Export a fresh XCP Studio toolchain.");
}

public sealed class StudioToolchainException : InvalidOperationException
{
    public StudioToolchainException(
        string code,
        string message,
        string path,
        string expected,
        string actual,
        string correction)
        : base(message)
    {
        Code = code;
        Details = new StudioToolchainErrorDetails(
            "xcp-studio-toolchain-error-details-v1",
            "toolchain_resolution",
            path,
            expected,
            actual,
            correction);
    }

    public string Code { get; }

    public StudioToolchainErrorDetails Details { get; }
}

public sealed record StudioToolchainErrorDetails(
    string SchemaVersion,
    string Stage,
    string Path,
    string Expected,
    string Actual,
    string Correction);

public static class StudioToolchainResolver
{
    public const string EnvironmentVariable = "XCP_STUDIO_TOOLCHAIN_ROOT";
    public const string ManifestFileName = "xcp-agent-kit-manifest.json";
    public const string ExpectedKitId = "xcp.c5-c6.agent-kit";

    private static readonly string[] RequiredEntrypoints =
    [
        "lifecycle",
        "project_builder",
        "source_adaptation",
        "project_evolution",
        "studio_live_adapter",
    ];

    public static StudioToolchain Resolve(
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
            Path.Combine(applicationBaseDirectory, "toolchain"),
            Path.Combine(
                localApplicationData,
                "XCP",
                "Studio",
                "toolchain"),
        };
        foreach (var candidate in candidates)
        {
            if (File.Exists(Path.Combine(candidate, ManifestFileName)))
            {
                return Load(candidate);
            }
        }

        throw new StudioToolchainException(
            "xcp.studio.toolchain_not_found",
            "No portable XCP Studio toolchain was found.",
            applicationBaseDirectory,
            $"{EnvironmentVariable}, application toolchain, or per-user toolchain",
            "none",
            $"Set {EnvironmentVariable} to an exact exported kit.");
    }

    public static StudioToolchain Load(string root)
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
                "xcp.studio.toolchain_manifest_invalid",
                manifestPath,
                "strict UTF-8 JSON xcp-agent-kit-manifest-v1",
                exception.GetType().Name,
                "Export a fresh exact toolchain.");
        }

        using (document)
        {
            var manifest = document.RootElement;
            RequireString(
                manifest,
                "schema_version",
                "xcp-agent-kit-manifest-v1",
                manifestPath);
            var kitId = RequireString(manifest, "kit_id", manifestPath);
            if (!string.Equals(
                    kitId,
                    ExpectedKitId,
                    StringComparison.Ordinal))
            {
                throw Invalid(
                    "xcp.studio.toolchain_identity_invalid",
                    manifestPath,
                    ExpectedKitId,
                    kitId,
                    "Select the canonical C5+C6+C7 agent kit.");
            }

            var kitVersion = RequireString(
                manifest,
                "kit_version",
                manifestPath);
            if (!Version.TryParse(kitVersion, out _))
            {
                throw Invalid(
                    "xcp.studio.toolchain_version_invalid",
                    manifestPath,
                    "numeric semantic version",
                    kitVersion,
                    "Export a versioned canonical toolchain.");
            }

            ValidateSecurity(manifest, manifestPath);
            var entrypoints = ReadEntrypoints(
                manifest,
                canonicalRoot,
                manifestPath);
            ValidateFiles(manifest, canonicalRoot, manifestPath);

            return new StudioToolchain(
                canonicalRoot,
                kitId,
                kitVersion,
                Convert.ToHexString(
                    SHA256.HashData(File.ReadAllBytes(manifestPath)))
                    .ToLowerInvariant(),
                entrypoints);
        }
    }

    private static IReadOnlyDictionary<string, string> ReadEntrypoints(
        JsonElement manifest,
        string root,
        string manifestPath)
    {
        if (!manifest.TryGetProperty("entrypoints", out var published) ||
            published.ValueKind != JsonValueKind.Object)
        {
            throw Invalid(
                "xcp.studio.toolchain_entrypoints_invalid",
                manifestPath,
                "entrypoints object",
                published.ValueKind.ToString(),
                "Export a complete Studio toolchain.");
        }

        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var name in RequiredEntrypoints)
        {
            var relative = RequireString(published, name, manifestPath);
            var fullPath = ResolveContainedPath(root, relative, manifestPath);
            if (!File.Exists(fullPath) ||
                (File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
            {
                throw Invalid(
                    "xcp.studio.toolchain_entrypoint_invalid",
                    fullPath,
                    "regular manifest-bound file",
                    File.Exists(fullPath) ? "reparse point" : "missing",
                    "Reject the kit and export exact bytes again.");
            }
            result[name] = fullPath;
        }
        return result;
    }

    private static void ValidateSecurity(
        JsonElement manifest,
        string manifestPath)
    {
        if (!manifest.TryGetProperty("security", out var security) ||
            security.ValueKind != JsonValueKind.Object)
        {
            throw Invalid(
                "xcp.studio.toolchain_security_invalid",
                manifestPath,
                "explicit fail-closed security object",
                security.ValueKind.ToString(),
                "Export a canonical toolchain.");
        }

        foreach (var field in new[]
                 {
                     "repository_metadata_included",
                     "samples_included",
                     "credentials_included",
                     "session_material_persisted",
                 })
        {
            if (!security.TryGetProperty(field, out var value) ||
                value.ValueKind is not JsonValueKind.False)
            {
                throw Invalid(
                    "xcp.studio.toolchain_security_invalid",
                    manifestPath,
                    $"{field}=false",
                    value.ValueKind.ToString(),
                    "Reject the kit and export a credential-free exact toolchain.");
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
                "xcp.studio.toolchain_files_invalid",
                manifestPath,
                "files array",
                files.ValueKind.ToString(),
                "Export a complete canonical toolchain.");
        }

        var expected = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in files.EnumerateArray())
        {
            var relative = RequireString(entry, "path", manifestPath);
            if (!expected.Add(relative))
            {
                throw Invalid(
                    "xcp.studio.toolchain_files_invalid",
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
                    "xcp.studio.toolchain_file_identity_mismatch",
                    path,
                    "regular manifest-bound file",
                    File.Exists(path) ? "reparse point" : "missing",
                    "Reject the kit and export exact bytes again.");
            }
            if (!entry.TryGetProperty("bytes", out var bytes) ||
                !bytes.TryGetInt64(out var expectedBytes) ||
                expectedBytes < 0)
            {
                throw Invalid(
                    "xcp.studio.toolchain_files_invalid",
                    manifestPath,
                    "non-negative byte length",
                    "invalid",
                    "Export a canonical toolchain.");
            }
            var actualBytes = new FileInfo(path).Length;
            var expectedHash = RequireString(entry, "sha256", manifestPath);
            var actualHash = Convert.ToHexString(
                SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
            if (actualBytes != expectedBytes ||
                !string.Equals(
                    actualHash,
                    expectedHash,
                    StringComparison.Ordinal))
            {
                throw Invalid(
                    "xcp.studio.toolchain_file_identity_mismatch",
                    path,
                    $"{expectedBytes} bytes / {expectedHash}",
                    $"{actualBytes} bytes / {actualHash}",
                    "Reject the modified kit.");
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
                "xcp.studio.toolchain_file_set_mismatch",
                root,
                string.Join(",", expected.Order()),
                string.Join(",", actual.Order()),
                "Reject the modified kit.");
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
                "xcp.studio.toolchain_path_invalid",
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
                "xcp.studio.toolchain_path_invalid",
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
                "xcp.studio.toolchain_manifest_invalid",
                path,
                $"non-empty string {name}",
                value.ValueKind.ToString(),
                "Export a fresh exact toolchain.");
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
                "xcp.studio.toolchain_manifest_invalid",
                path,
                expected,
                actual,
                "Export a fresh exact toolchain.");
        }
    }

    private static StudioToolchainException Invalid(
        string code,
        string path,
        string expected,
        string actual,
        string correction) =>
        new(
            code,
            "The portable XCP Studio toolchain failed closed.",
            path,
            expected,
            actual,
            correction);
}
