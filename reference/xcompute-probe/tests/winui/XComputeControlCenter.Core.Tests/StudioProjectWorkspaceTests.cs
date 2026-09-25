using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class StudioProjectWorkspaceTests
{
    [Fact]
    public async Task GuidedAndExpertViewsEditTheSameProjectFile()
    {
        var root = NewTemporaryRoot();
        try
        {
            var projectDirectory = Path.Combine(root, "same project file");
            var projectClient = CreateProjectClient();
            Assert.True(
                (await projectClient.CreateAsync(
                    projectDirectory,
                    "studio-same-file",
                    "Studio Same File")).Ok);

            var workspace = new StudioProjectWorkspace();
            var opened = await workspace.OpenAsync(projectDirectory);
            var guided = await workspace.SaveGuidedAsync(
                projectDirectory,
                opened.FileSha256,
                "Guided Title",
                "Guided description.",
                "1.1.0");

            Assert.Equal(opened.ProjectPath, guided.ProjectPath);
            Assert.Equal("Guided Title", guided.Title);
            Assert.Equal("Guided description.", guided.Description);
            Assert.Equal("1.1.0", guided.Version);
            Assert.NotEqual(opened.FileSha256, guided.FileSha256);

            var expertSource = guided.SourceText.Replace(
                "Guided description.",
                "Expert description.",
                StringComparison.Ordinal);
            var expert = await workspace.SaveSourceAsync(
                projectDirectory,
                guided.FileSha256,
                expertSource);

            Assert.Equal(opened.ProjectPath, expert.ProjectPath);
            Assert.Equal("Guided Title", expert.Title);
            Assert.Equal("Expert description.", expert.Description);
            Assert.NotEqual(guided.FileSha256, expert.FileSha256);
            Assert.True(
                (await projectClient.ValidateAsync(projectDirectory)).Ok);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task StaleRevisionFailsClosedWithoutOverwritingExternalEdit()
    {
        var root = NewTemporaryRoot();
        try
        {
            var projectDirectory = Path.Combine(root, "stale revision");
            Assert.True(
                (await CreateProjectClient().CreateAsync(
                    projectDirectory,
                    "studio-stale-revision",
                    "Studio Stale Revision")).Ok);
            var workspace = new StudioProjectWorkspace();
            var opened = await workspace.OpenAsync(projectDirectory);
            var external = opened.SourceText.Replace(
                "Studio Stale Revision",
                "External Writer",
                StringComparison.Ordinal);
            await File.WriteAllTextAsync(opened.ProjectPath, external);

            var error = await Assert.ThrowsAsync<StudioWorkspaceException>(
                () => workspace.SaveGuidedAsync(
                    projectDirectory,
                    opened.FileSha256,
                    "Studio Overwrite",
                    opened.Description,
                    opened.Version));

            Assert.Equal("xcp.studio.project_changed", error.Code);
            Assert.Equal("reopen the project and reconcile the newer file",
                error.Details.Correction);
            Assert.Equal(external, await File.ReadAllTextAsync(opened.ProjectPath));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task InvalidExpertJsonFailsBeforeReplacingTheProject()
    {
        var root = NewTemporaryRoot();
        try
        {
            var projectDirectory = Path.Combine(root, "invalid expert");
            Assert.True(
                (await CreateProjectClient().CreateAsync(
                    projectDirectory,
                    "studio-invalid-expert",
                    "Studio Invalid Expert")).Ok);
            var workspace = new StudioProjectWorkspace();
            var opened = await workspace.OpenAsync(projectDirectory);

            var error = await Assert.ThrowsAsync<StudioWorkspaceException>(
                () => workspace.SaveSourceAsync(
                    projectDirectory,
                    opened.FileSha256,
                    "{ invalid"));

            Assert.Equal("xcp.studio.project_json_invalid", error.Code);
            Assert.Equal(opened.SourceText,
                await File.ReadAllTextAsync(opened.ProjectPath));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static XcpCreativeCliClient CreateProjectClient()
    {
        var root = FindRepositoryRoot();
        return new XcpCreativeCliClient(
            Path.Combine(root, "tools", "xcp_creative_project.py"));
    }

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(
                    Path.Combine(
                        current.FullName,
                        "tools",
                        "xcp_creative_project.py")))
            {
                return current.FullName;
            }
            current = current.Parent;
        }
        throw new DirectoryNotFoundException(
            "Could not locate the xcompute-probe repository root.");
    }

    private static string NewTemporaryRoot()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-workspace-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }
}
