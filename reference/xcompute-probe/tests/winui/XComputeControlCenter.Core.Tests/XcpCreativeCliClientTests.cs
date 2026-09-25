using System.Text.Json;
using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class XcpCreativeCliClientTests
{
    [Fact]
    public async Task DescribeBindsTheAuthoritativePortableBuilder()
    {
        var client = CreateClient();
        var result = await client.DescribeAsync();

        Assert.True(result.Ok);
        Assert.Equal("xcp-creative-builder-description-v1", result.SchemaVersion);
        var commands = result.Payload.GetProperty("commands")
            .EnumerateArray()
            .Select(item => item.GetString())
            .ToArray();
        Assert.Contains("create", commands);
        Assert.Contains("validate", commands);
        Assert.Contains("build", commands);
        Assert.Contains("verify", commands);
    }

    [Fact]
    public async Task CompleteLocalLifecycleUsesPathsWithSpaces()
    {
        var client = CreateClient();
        var root = NewTemporaryRoot();
        try
        {
            var project = Path.Combine(root, "project with spaces");
            var bundle = Path.Combine(root, "bundle with spaces");

            var created = await client.CreateAsync(
                project,
                "studio-client-project",
                "Studio Client Project");
            Assert.True(created.Ok);
            Assert.Equal("xcp-creative-project-create-v1", created.SchemaVersion);

            var validated = await client.ValidateAsync(project);
            Assert.True(validated.Ok);
            Assert.Equal(
                "studio-client-project",
                validated.Payload.GetProperty("project_id").GetString());

            var built = await client.BuildAsync(project, bundle);
            Assert.True(built.Ok);
            Assert.Equal("xcp-creative-bundle-build-v1", built.SchemaVersion);

            var verified = await client.VerifyAsync(bundle);
            Assert.True(verified.Ok);
            Assert.Equal(
                "xcp-creative-bundle-verification-v1",
                verified.SchemaVersion);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task StructuredFailureIsPreservedWithoutMessageBranching()
    {
        var client = CreateClient();
        var root = NewTemporaryRoot();
        try
        {
            var project = Path.Combine(root, "rejected");
            var result = await client.CreateAsync(
                project,
                "bad;project",
                "Rejected Project");

            Assert.False(result.Ok);
            Assert.NotNull(result.Error);
            Assert.Equal(
                "xcp.creative.schema_rejected",
                result.Error!.Code);
            Assert.Equal(
                "xcp-creative-error-details-v1",
                result.Error.Details!.SchemaVersion);
            Assert.False(Directory.Exists(project));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task ExistingProjectIsNeverOverwritten()
    {
        var client = CreateClient();
        var root = NewTemporaryRoot();
        try
        {
            var project = Path.Combine(root, "existing");
            Directory.CreateDirectory(project);
            var marker = Path.Combine(project, "keep.txt");
            await File.WriteAllTextAsync(marker, "keep\n");

            var result = await client.CreateAsync(
                project,
                "existing-project",
                "Existing Project");

            Assert.False(result.Ok);
            Assert.Equal("xcp.creative.output_exists", result.Error!.Code);
            Assert.Equal("keep\n", await File.ReadAllTextAsync(marker));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static XcpCreativeCliClient CreateClient()
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
            var candidate = Path.Combine(
                current.FullName,
                "tools",
                "xcp_creative_project.py");
            if (File.Exists(candidate))
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
            $"xcp-studio-core-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }
}
