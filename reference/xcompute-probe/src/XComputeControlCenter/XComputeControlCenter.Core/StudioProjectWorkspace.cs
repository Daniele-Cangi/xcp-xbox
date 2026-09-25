using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace XComputeControlCenter.Core;

public sealed class StudioProjectWorkspace : IStudioProjectWorkspace
{
    public const string ProjectFileName = "xcp-project.json";

    public async Task<StudioProjectSnapshot> OpenAsync(
        string projectDirectory,
        CancellationToken cancellationToken = default)
    {
        var projectPath = ResolveProjectPath(projectDirectory);
        byte[] bytes;
        try
        {
            bytes = await File.ReadAllBytesAsync(
                projectPath,
                cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (
            exception is FileNotFoundException or DirectoryNotFoundException)
        {
            throw Error(
                "xcp.studio.project_missing",
                "The selected directory contains no xcp-project.json.",
                "open",
                projectPath,
                "existing xcp-project.json",
                "missing",
                "select or create an XCP project directory",
                exception);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException)
        {
            throw Error(
                "xcp.studio.project_read_failed",
                "Studio could not read xcp-project.json.",
                "open",
                projectPath,
                "readable xcp-project.json",
                exception.GetType().Name,
                "verify file access and retry",
                exception);
        }

        return ParseSnapshot(
            Path.GetFullPath(projectDirectory),
            projectPath,
            bytes);
    }

    public async Task<StudioProjectSnapshot> SaveGuidedAsync(
        string projectDirectory,
        string expectedSha256,
        string title,
        string description,
        string version,
        CancellationToken cancellationToken = default)
    {
        var current = await ReadForUpdateAsync(
            projectDirectory,
            expectedSha256,
            cancellationToken).ConfigureAwait(false);
        JsonObject root;
        try
        {
            root = JsonNode.Parse(current.SourceText) as JsonObject
                ?? throw new JsonException("The project root is not an object.");
        }
        catch (JsonException exception)
        {
            throw InvalidJson(current.ProjectPath, exception);
        }

        root["title"] = title;
        root["description"] = description;
        root["version"] = version;
        var source = root.ToJsonString(
            new JsonSerializerOptions { WriteIndented = true }) + "\n";
        return await SaveValidatedSourceAsync(
            current,
            source,
            cancellationToken).ConfigureAwait(false);
    }

    public async Task<StudioProjectSnapshot> SaveSourceAsync(
        string projectDirectory,
        string expectedSha256,
        string sourceText,
        CancellationToken cancellationToken = default)
    {
        var current = await ReadForUpdateAsync(
            projectDirectory,
            expectedSha256,
            cancellationToken).ConfigureAwait(false);
        return await SaveValidatedSourceAsync(
            current,
            sourceText,
            cancellationToken).ConfigureAwait(false);
    }

    private static async Task<StudioProjectSnapshot> ReadForUpdateAsync(
        string projectDirectory,
        string expectedSha256,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(expectedSha256))
        {
            throw Error(
                "xcp.studio.project_revision_required",
                "Saving requires the SHA-256 observed when the project was opened.",
                "edit",
                ResolveProjectPath(projectDirectory),
                "64 lowercase hexadecimal characters",
                "missing",
                "open the project again before saving");
        }

        var workspace = new StudioProjectWorkspace();
        var current = await workspace.OpenAsync(
            projectDirectory,
            cancellationToken).ConfigureAwait(false);
        if (!current.FileSha256.Equals(
                expectedSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            throw Error(
                "xcp.studio.project_changed",
                "The project changed after it was opened.",
                "edit",
                current.ProjectPath,
                expectedSha256.ToLowerInvariant(),
                current.FileSha256,
                "reopen the project and reconcile the newer file");
        }
        return current;
    }

    private static async Task<StudioProjectSnapshot> SaveValidatedSourceAsync(
        StudioProjectSnapshot current,
        string sourceText,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(sourceText);
        var normalized = sourceText.EndsWith('\n')
            ? sourceText
            : sourceText + "\n";
        var bytes = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false)
            .GetBytes(normalized);
        _ = ParseSnapshot(
            current.ProjectDirectory,
            current.ProjectPath,
            bytes);

        var temporaryPath =
            $"{current.ProjectPath}.studio-{Guid.NewGuid():N}.tmp";
        try
        {
            await File.WriteAllBytesAsync(
                temporaryPath,
                bytes,
                cancellationToken).ConfigureAwait(false);
            var latestBytes = await File.ReadAllBytesAsync(
                current.ProjectPath,
                cancellationToken).ConfigureAwait(false);
            var latestSha = Convert.ToHexString(
                SHA256.HashData(latestBytes)).ToLowerInvariant();
            if (!latestSha.Equals(
                    current.FileSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                TryDelete(temporaryPath);
                throw Error(
                    "xcp.studio.project_changed",
                    "The project changed while Studio was preparing the save.",
                    "edit",
                    current.ProjectPath,
                    current.FileSha256,
                    latestSha,
                    "reopen the project and reconcile the newer file");
            }
            File.Move(temporaryPath, current.ProjectPath, overwrite: true);
        }
        catch (StudioWorkspaceException)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException)
        {
            TryDelete(temporaryPath);
            throw Error(
                "xcp.studio.project_write_failed",
                "Studio could not atomically replace xcp-project.json.",
                "edit",
                current.ProjectPath,
                "atomic replacement",
                exception.GetType().Name,
                "close competing writers, verify access, and retry",
                exception);
        }

        return ParseSnapshot(
            current.ProjectDirectory,
            current.ProjectPath,
            bytes);
    }

    private static StudioProjectSnapshot ParseSnapshot(
        string projectDirectory,
        string projectPath,
        byte[] bytes)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(bytes);
        }
        catch (JsonException exception)
        {
            throw InvalidJson(projectPath, exception);
        }

        using (document)
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw Error(
                    "xcp.studio.project_shape_invalid",
                    "The project document root must be a JSON object.",
                    "edit",
                    projectPath,
                    "JSON object",
                    document.RootElement.ValueKind.ToString(),
                    "repair the source and validate it with the portable CLI");
            }

            return new StudioProjectSnapshot(
                projectDirectory,
                projectPath,
                RequiredString(document.RootElement, "project_id", projectPath),
                RequiredString(document.RootElement, "version", projectPath),
                RequiredString(document.RootElement, "title", projectPath),
                RequiredString(document.RootElement, "description", projectPath),
                Encoding.UTF8.GetString(bytes),
                Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
        }
    }

    private static string RequiredString(
        JsonElement root,
        string propertyName,
        string projectPath)
    {
        if (!root.TryGetProperty(propertyName, out var value) ||
            value.ValueKind != JsonValueKind.String)
        {
            throw Error(
                "xcp.studio.project_shape_invalid",
                $"The project is missing string field {propertyName}.",
                "edit",
                projectPath,
                $"string field {propertyName}",
                "missing or non-string",
                "repair the source and validate it with the portable CLI");
        }
        return value.GetString() ?? string.Empty;
    }

    private static string ResolveProjectPath(string projectDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(projectDirectory);
        return Path.Combine(
            Path.GetFullPath(projectDirectory),
            ProjectFileName);
    }

    private static StudioWorkspaceException InvalidJson(
        string path,
        JsonException exception) =>
        Error(
            "xcp.studio.project_json_invalid",
            "The project source is not valid JSON.",
            "edit",
            path,
            "valid JSON object",
            $"{exception.LineNumber}:{exception.BytePositionInLine}",
            "repair the JSON syntax before saving",
            exception);

    private static StudioWorkspaceException Error(
        string code,
        string message,
        string stage,
        string path,
        string expected,
        string actual,
        string correction,
        Exception? innerException = null) =>
        new(
            code,
            message,
            new StudioWorkspaceErrorDetails(
                stage,
                path,
                expected,
                actual,
                correction),
            innerException);

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch
        {
            // A failed cleanup must not hide the structured write failure.
        }
    }
}
