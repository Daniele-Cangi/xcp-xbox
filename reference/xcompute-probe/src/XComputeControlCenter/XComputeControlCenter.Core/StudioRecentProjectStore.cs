using System.Text.Json;
using System.Text.Json.Serialization;

namespace XComputeControlCenter.Core;

public enum StudioRecentProjectStatus
{
    Available,
    Missing,
    Moved,
}

public sealed record StudioRecentProjectEntry(
    string ProjectId,
    string Title,
    string Version,
    string OriginalDirectory,
    string ProjectDirectory,
    string Provenance,
    DateTimeOffset LastOpenedUtc,
    StudioRecentProjectStatus Status)
{
    public string StatusLabel => Status.ToString().ToUpperInvariant();

    public string LocationLabel =>
        Status is StudioRecentProjectStatus.Moved
            ? $"{OriginalDirectory} → {ProjectDirectory}"
            : ProjectDirectory;
}

public interface IStudioRecentProjectStore
{
    Task<IReadOnlyList<StudioRecentProjectEntry>> LoadAsync(
        CancellationToken cancellationToken = default);

    Task<IReadOnlyList<StudioRecentProjectEntry>> RecordAsync(
        StudioProjectSnapshot snapshot,
        string provenance,
        CancellationToken cancellationToken = default);

    Task<IReadOnlyList<StudioRecentProjectEntry>> RelinkAsync(
        string originalDirectory,
        StudioProjectSnapshot relocatedProject,
        CancellationToken cancellationToken = default);
}

public sealed class StudioRecentProjectStore : IStudioRecentProjectStore
{
    public const string SchemaVersion = "xcp-studio-recent-projects-v1";
    public const string DefaultFileName = "recent-projects-v1.json";
    private const int MaximumEntries = 20;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    private readonly string _storePath;
    private readonly Func<DateTimeOffset> _utcNow;

    public StudioRecentProjectStore(
        string storePath,
        Func<DateTimeOffset>? utcNow = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(storePath);
        _storePath = Path.GetFullPath(storePath);
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
    }

    public static StudioRecentProjectStore CreateDefault() =>
        new(Path.Combine(
            Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData),
            "XCP",
            "Studio",
            DefaultFileName));

    public async Task<IReadOnlyList<StudioRecentProjectEntry>> LoadAsync(
        CancellationToken cancellationToken = default)
    {
        var document = await ReadAsync(cancellationToken).ConfigureAwait(false);
        return Project(document.Entries);
    }

    public async Task<IReadOnlyList<StudioRecentProjectEntry>> RecordAsync(
        StudioProjectSnapshot snapshot,
        string provenance,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        ArgumentException.ThrowIfNullOrWhiteSpace(provenance);
        var document = await ReadAsync(cancellationToken).ConfigureAwait(false);
        var directory = Canonical(snapshot.ProjectDirectory);
        var existing = document.Entries.FirstOrDefault(entry =>
            SamePath(entry.ProjectDirectory, directory));
        document.Entries.RemoveAll(entry =>
            SamePath(entry.ProjectDirectory, directory));
        document.Entries.Add(new PersistedEntry(
            snapshot.ProjectId,
            snapshot.Title,
            snapshot.Version,
            existing?.OriginalDirectory ?? directory,
            directory,
            existing?.Provenance ?? provenance,
            _utcNow()));
        await WriteAsync(document, cancellationToken).ConfigureAwait(false);
        return Project(document.Entries);
    }

    public async Task<IReadOnlyList<StudioRecentProjectEntry>> RelinkAsync(
        string originalDirectory,
        StudioProjectSnapshot relocatedProject,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(originalDirectory);
        ArgumentNullException.ThrowIfNull(relocatedProject);
        var document = await ReadAsync(cancellationToken).ConfigureAwait(false);
        var original = Canonical(originalDirectory);
        var entry = document.Entries.FirstOrDefault(candidate =>
            SamePath(candidate.OriginalDirectory, original) ||
            SamePath(candidate.ProjectDirectory, original));
        if (entry is null)
        {
            throw new StudioRecentProjectStoreException(
                "xcp.studio.recent_project_not_found",
                "The selected recent-project record no longer exists.",
                original,
                "existing recent-project path",
                "not found",
                "Refresh the project library and select an existing record.");
        }

        document.Entries.Remove(entry);
        document.Entries.RemoveAll(candidate =>
            SamePath(
                candidate.ProjectDirectory,
                relocatedProject.ProjectDirectory));
        document.Entries.Add(new PersistedEntry(
            relocatedProject.ProjectId,
            relocatedProject.Title,
            relocatedProject.Version,
            entry.OriginalDirectory,
            Canonical(relocatedProject.ProjectDirectory),
            $"{entry.Provenance};relinked",
            _utcNow()));
        await WriteAsync(document, cancellationToken).ConfigureAwait(false);
        return Project(document.Entries);
    }

    private async Task<PersistedDocument> ReadAsync(
        CancellationToken cancellationToken)
    {
        if (!File.Exists(_storePath))
        {
            return new PersistedDocument(SchemaVersion, []);
        }
        try
        {
            await using var stream = new FileStream(
                _storePath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                4096,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            var document = await JsonSerializer.DeserializeAsync<PersistedDocument>(
                stream,
                JsonOptions,
                cancellationToken).ConfigureAwait(false);
            if (document is null ||
                !string.Equals(
                    document.SchemaVersion,
                    SchemaVersion,
                    StringComparison.Ordinal) ||
                document.Entries is null)
            {
                throw new JsonException("Recent-project schema mismatch.");
            }
            return document;
        }
        catch (Exception exception)
            when (exception is IOException or UnauthorizedAccessException or
                  JsonException)
        {
            throw new StudioRecentProjectStoreException(
                "xcp.studio.recent_projects_invalid",
                "The recent-project library failed closed.",
                _storePath,
                SchemaVersion,
                exception.GetType().Name,
                "Move the invalid metadata file aside and reopen Studio.");
        }
    }

    private async Task WriteAsync(
        PersistedDocument document,
        CancellationToken cancellationToken)
    {
        document.Entries = document.Entries
            .OrderByDescending(entry => entry.LastOpenedUtc)
            .Take(MaximumEntries)
            .ToList();
        var directory = Path.GetDirectoryName(_storePath)!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(
            directory,
            $".{Path.GetFileName(_storePath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            await using (var stream = new FileStream(
                             temporary,
                             FileMode.CreateNew,
                             FileAccess.Write,
                             FileShare.None,
                             4096,
                             FileOptions.Asynchronous |
                             FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(
                    stream,
                    document,
                    JsonOptions,
                    cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            }
            File.Move(temporary, _storePath, true);
        }
        finally
        {
            if (File.Exists(temporary))
            {
                File.Delete(temporary);
            }
        }
    }

    private static IReadOnlyList<StudioRecentProjectEntry> Project(
        IEnumerable<PersistedEntry> entries) =>
        entries
            .OrderByDescending(entry => entry.LastOpenedUtc)
            .Select(entry =>
            {
                var available = File.Exists(Path.Combine(
                    entry.ProjectDirectory,
                    StudioProjectWorkspace.ProjectFileName));
                var moved = available &&
                    !SamePath(
                        entry.OriginalDirectory,
                        entry.ProjectDirectory);
                return new StudioRecentProjectEntry(
                    entry.ProjectId,
                    entry.Title,
                    entry.Version,
                    entry.OriginalDirectory,
                    entry.ProjectDirectory,
                    entry.Provenance,
                    entry.LastOpenedUtc,
                    moved
                        ? StudioRecentProjectStatus.Moved
                        : available
                            ? StudioRecentProjectStatus.Available
                            : StudioRecentProjectStatus.Missing);
            })
            .ToArray();

    private static string Canonical(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private static bool SamePath(string left, string right) =>
        string.Equals(
            Canonical(left),
            Canonical(right),
            StringComparison.OrdinalIgnoreCase);

    private sealed record PersistedEntry(
        string ProjectId,
        string Title,
        string Version,
        string OriginalDirectory,
        string ProjectDirectory,
        string Provenance,
        DateTimeOffset LastOpenedUtc);

    private sealed class PersistedDocument
    {
        [JsonConstructor]
        public PersistedDocument(
            string schemaVersion,
            List<PersistedEntry> entries)
        {
            SchemaVersion = schemaVersion;
            Entries = entries;
        }

        public string SchemaVersion { get; }

        public List<PersistedEntry> Entries { get; set; }
    }
}

public sealed class StudioRecentProjectStoreException : InvalidOperationException
{
    public StudioRecentProjectStoreException(
        string code,
        string message,
        string path,
        string expected,
        string actual,
        string correction)
        : base(message)
    {
        Code = code;
        Details = new StudioWorkspaceErrorDetails(
            "recent_projects",
            path,
            expected,
            actual,
            correction);
    }

    public string Code { get; }

    public StudioWorkspaceErrorDetails Details { get; }
}
