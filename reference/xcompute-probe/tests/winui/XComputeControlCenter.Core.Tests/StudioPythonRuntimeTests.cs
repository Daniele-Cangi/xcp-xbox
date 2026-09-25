using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class StudioPythonRuntimeTests
{
    [Fact]
    public void LoadAcceptsExactPrivateRuntime()
    {
        using var fixture = RuntimeFixture.Create();

        var runtime = StudioPythonRuntimeResolver.Load(fixture.Root);

        Assert.Equal("xcp.studio.python", runtime.RuntimeId);
        Assert.Equal("3.13.15", runtime.RuntimeVersion);
        Assert.Equal("x64", runtime.Architecture);
        Assert.Equal(Path.Combine(fixture.Root, "python.exe"), runtime.Executable);
        Assert.Equal(64, runtime.ManifestSha256.Length);
    }

    [Fact]
    public void LoadRejectsTamperedRuntimeBytes()
    {
        using var fixture = RuntimeFixture.Create();
        File.AppendAllText(
            Path.Combine(fixture.Root, "python313.zip"),
            "tampered");

        var error = Assert.Throws<StudioPythonRuntimeException>(
            () => StudioPythonRuntimeResolver.Load(fixture.Root));

        Assert.Equal(
            "xcp.studio.python_runtime_file_identity_mismatch",
            error.Code);
    }

    [Fact]
    public void ResolveUsesApplicationAdjacentRuntimeBeforePerUserRuntime()
    {
        using var fixture = RuntimeFixture.Create();
        var application = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-app-{Guid.NewGuid():N}");
        var adjacent = Path.Combine(application, "runtime", "python");
        Directory.CreateDirectory(Path.GetDirectoryName(adjacent)!);
        Directory.Move(fixture.Root, adjacent);
        fixture.Root = adjacent;
        try
        {
            var runtime = StudioPythonRuntimeResolver.Resolve(
                application,
                configuredRoot: null,
                localApplicationData: Path.Combine(application, "local"));

            Assert.Equal(Path.GetFullPath(adjacent), runtime.Root);
        }
        finally
        {
            Directory.Delete(application, true);
            fixture.Root = string.Empty;
        }
    }

    private sealed class RuntimeFixture : IDisposable
    {
        private RuntimeFixture(string root)
        {
            Root = root;
        }

        public string Root { get; set; }

        public static RuntimeFixture Create()
        {
            var root = Path.Combine(
                Path.GetTempPath(),
                $"xcp-studio-python-{Guid.NewGuid():N}");
            Directory.CreateDirectory(root);
            var files = new Dictionary<string, byte[]>
            {
                ["python.exe"] = Encoding.UTF8.GetBytes("fixture executable"),
                ["python313.zip"] = Encoding.UTF8.GetBytes("fixture stdlib"),
            };
            foreach (var (relative, bytes) in files)
            {
                File.WriteAllBytes(Path.Combine(root, relative), bytes);
            }
            var manifest = new
            {
                schema_version =
                    "xcp-studio-python-runtime-manifest-v1",
                runtime_id = "xcp.studio.python",
                runtime_version = "3.13.15",
                architecture = "x64",
                executable = "python.exe",
                security = new
                {
                    credentials_included = false,
                    session_material_persisted = false,
                    user_site_enabled = false,
                },
                files = files.Select(item => new
                {
                    path = item.Key,
                    bytes = item.Value.Length,
                    sha256 = Convert.ToHexString(
                        SHA256.HashData(item.Value)).ToLowerInvariant(),
                }),
            };
            File.WriteAllText(
                Path.Combine(
                    root,
                    StudioPythonRuntimeResolver.ManifestFileName),
                JsonSerializer.Serialize(manifest));
            return new RuntimeFixture(root);
        }

        public void Dispose()
        {
            if (!string.IsNullOrEmpty(Root) && Directory.Exists(Root))
            {
                Directory.Delete(Root, true);
            }
        }
    }
}
