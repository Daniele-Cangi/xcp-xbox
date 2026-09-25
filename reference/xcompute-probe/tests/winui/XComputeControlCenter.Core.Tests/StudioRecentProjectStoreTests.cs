using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class StudioRecentProjectStoreTests
{
    [Fact]
    public async Task RecordProjectsAvailableAndMissingStateWithoutCreativeCopy()
    {
        using var fixture = new RecentProjectFixture();
        var project = fixture.CreateProject("first", "First");
        var store = fixture.CreateStore();

        var available = await store.RecordAsync(project, "opened");

        Assert.Single(available);
        Assert.Equal(
            StudioRecentProjectStatus.Available,
            available[0].Status);
        Assert.DoesNotContain(
            project.SourceText,
            await File.ReadAllTextAsync(fixture.StorePath));
        var refreshed = await store.RecordAsync(
            project with { Title = "Updated title" },
            "edited");
        Assert.Equal("opened", refreshed[0].Provenance);
        Assert.Equal("Updated title", refreshed[0].Title);

        File.Delete(project.ProjectPath);
        var missing = await store.LoadAsync();
        Assert.Equal(StudioRecentProjectStatus.Missing, missing[0].Status);
    }

    [Fact]
    public async Task RelinkPreservesOriginalPathAndProjectsMovedState()
    {
        using var fixture = new RecentProjectFixture();
        var original = fixture.CreateProject("move-me", "Move Me");
        var store = fixture.CreateStore();
        await store.RecordAsync(original, "created");
        File.Delete(original.ProjectPath);
        var relocated = fixture.CreateProject(
            "move-me",
            "Move Me",
            "relocated");

        var entries = await store.RelinkAsync(
            original.ProjectDirectory,
            relocated);

        var moved = Assert.Single(entries);
        Assert.Equal(StudioRecentProjectStatus.Moved, moved.Status);
        Assert.Equal(original.ProjectDirectory, moved.OriginalDirectory);
        Assert.Equal(relocated.ProjectDirectory, moved.ProjectDirectory);
        Assert.Contains("relinked", moved.Provenance);
    }

    [Fact]
    public async Task InvalidMetadataFailsClosed()
    {
        using var fixture = new RecentProjectFixture();
        Directory.CreateDirectory(Path.GetDirectoryName(fixture.StorePath)!);
        await File.WriteAllTextAsync(fixture.StorePath, "{}");
        var store = fixture.CreateStore();

        var error = await Assert.ThrowsAsync<StudioRecentProjectStoreException>(
            () => store.LoadAsync());

        Assert.Equal("xcp.studio.recent_projects_invalid", error.Code);
    }

    private sealed class RecentProjectFixture : IDisposable
    {
        private readonly string _root = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-recent-{Guid.NewGuid():N}");

        public RecentProjectFixture()
        {
            Directory.CreateDirectory(_root);
        }

        public string StorePath => Path.Combine(
            _root,
            "state",
            StudioRecentProjectStore.DefaultFileName);

        public StudioRecentProjectStore CreateStore() =>
            new(
                StorePath,
                () => new DateTimeOffset(
                    2026,
                    7,
                    29,
                    12,
                    0,
                    0,
                    TimeSpan.Zero));

        public StudioProjectSnapshot CreateProject(
            string id,
            string title,
            string directoryName = "project")
        {
            var directory = Path.Combine(_root, directoryName);
            Directory.CreateDirectory(directory);
            var projectPath = Path.Combine(
                directory,
                StudioProjectWorkspace.ProjectFileName);
            var source =
                $"{{\"project_id\":\"{id}\",\"version\":\"1.0.0\",\"title\":\"{title}\"}}";
            File.WriteAllText(projectPath, source);
            return new StudioProjectSnapshot(
                directory,
                projectPath,
                id,
                "1.0.0",
                title,
                "fixture",
                source,
                new string('a', 64));
        }

        public void Dispose()
        {
            Directory.Delete(_root, true);
        }
    }
}
