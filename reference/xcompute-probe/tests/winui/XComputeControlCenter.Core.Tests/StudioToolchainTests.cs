using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class StudioToolchainTests
{
    [Fact]
    public void LoadAcceptsExactVersionedManifestAndPublishedEntrypoints()
    {
        using var fixture = ToolchainFixture.Create();

        var toolchain = StudioToolchainResolver.Load(fixture.Root);

        Assert.Equal("xcp.c5-c6.agent-kit", toolchain.KitId);
        Assert.Equal("1.8.0", toolchain.KitVersion);
        Assert.Equal(64, toolchain.ManifestSha256.Length);
        Assert.Equal(
            Path.Combine(fixture.Root, "tools", "xcp_agent_lifecycle.py"),
            toolchain.Entrypoint("lifecycle"));
        Assert.Equal(
            Path.Combine(fixture.Root, "tools", "xcp_studio_live_adapter.py"),
            toolchain.Entrypoint("studio_live_adapter"));
    }

    [Fact]
    public void LoadRejectsTamperedToolBytes()
    {
        using var fixture = ToolchainFixture.Create();
        File.AppendAllText(
            Path.Combine(
                fixture.Root,
                "tools",
                "xcp_agent_lifecycle.py"),
            "# tampered\n");

        var error = Assert.Throws<StudioToolchainException>(
            () => StudioToolchainResolver.Load(fixture.Root));

        Assert.Equal(
            "xcp.studio.toolchain_file_identity_mismatch",
            error.Code);
    }

    [Fact]
    public void ResolveUsesExplicitPortableRootOutsideApplicationDirectory()
    {
        using var fixture = ToolchainFixture.Create();
        var unrelatedApplication = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-app-{Guid.NewGuid():N}");
        Directory.CreateDirectory(unrelatedApplication);
        try
        {
            var toolchain = StudioToolchainResolver.Resolve(
                unrelatedApplication,
                fixture.Root,
                Path.Combine(unrelatedApplication, "local"));

            Assert.Equal(Path.GetFullPath(fixture.Root), toolchain.Root);
        }
        finally
        {
            Directory.Delete(unrelatedApplication, true);
        }
    }

    private sealed class ToolchainFixture : IDisposable
    {
        private static readonly Dictionary<string, string> Entrypoints = new()
        {
            ["lifecycle"] = "tools/xcp_agent_lifecycle.py",
            ["blind_creation_finalizer"] = "tools/xcp_agent_blind_gate.py",
            ["project_builder"] = "tools/xcp_creative_project.py",
            ["source_adaptation"] = "tools/xcp_source_adapt.py",
            ["project_evolution"] = "tools/xcp_project_evolve.py",
            ["studio_live_adapter"] = "tools/xcp_studio_live_adapter.py",
        };

        private ToolchainFixture(string root)
        {
            Root = root;
        }

        public string Root { get; }

        public static ToolchainFixture Create()
        {
            var root = Path.Combine(
                Path.GetTempPath(),
                $"xcp-studio-toolchain-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Path.Combine(root, "tools"));
            var files = new List<object>();
            foreach (var relative in Entrypoints.Values.Distinct())
            {
                var path = Path.Combine(
                    root,
                    relative.Replace('/', Path.DirectorySeparatorChar));
                var bytes = Encoding.UTF8.GetBytes(
                    $"# exact fixture for {relative}\n");
                File.WriteAllBytes(path, bytes);
                files.Add(new
                {
                    path = relative,
                    bytes = bytes.Length,
                    sha256 = Convert.ToHexString(
                        SHA256.HashData(bytes)).ToLowerInvariant(),
                });
            }

            var manifest = new
            {
                schema_version = "xcp-agent-kit-manifest-v1",
                kit_id = "xcp.c5-c6.agent-kit",
                kit_version = "1.8.0",
                gate_id = "C5_C6_AGENT_NATIVE_CREATIVE_GATE",
                entrypoints = Entrypoints,
                runtime = new
                {
                    python_minimum = "3.11",
                    requirements = "requirements.txt",
                },
                security = new
                {
                    repository_metadata_included = false,
                    samples_included = false,
                    credentials_included = false,
                    session_material_persisted = false,
                },
                files,
            };
            File.WriteAllText(
                Path.Combine(
                    root,
                    StudioToolchainResolver.ManifestFileName),
                JsonSerializer.Serialize(manifest));
            return new ToolchainFixture(root);
        }

        public void Dispose()
        {
            Directory.Delete(Root, true);
        }
    }
}
