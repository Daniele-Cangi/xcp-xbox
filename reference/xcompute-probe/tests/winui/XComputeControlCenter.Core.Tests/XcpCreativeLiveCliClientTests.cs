using System.Text.Json;
using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class XcpCreativeLiveCliClientTests
{
    [Fact]
    public async Task TrustedProviderInjectsOnlyATemporarySessionAndClosesIt()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            $"xcp-studio-trust-client-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            var adapter = Path.Combine(root, "adapter.py");
            await File.WriteAllTextAsync(
                adapter,
                "import json, os, sys\n" +
                "sys.stdin.read()\n" +
                "assert os.environ.get('XCP_SESSION_ID') == 'ephemeral-test-session'\n" +
                "assert not os.environ.get('XCP_PAIRING_CODE')\n" +
                "print(json.dumps({'ok': True, 'schema_version': 'worker-sdk-result-v1', 'payload': {}}))\n");
            var provider = new FakeSessionProvider();
            var client = new XcpCreativeLiveCliClient(
                adapter,
                "127.0.0.1",
                timeoutSeconds: 1,
                inheritAuthenticationEnvironment: false,
                sessionProvider: provider);

            var result = await client.DescribeCreativeHostAsync();

            Assert.True(result.Ok);
            Assert.True(provider.Opened);
            Assert.True(provider.Disposed);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task AdapterDescriptionPublishesTheExactCreativeSurface()
    {
        var result = await CreateClient().DescribeAdapterAsync();

        Assert.True(result.Ok);
        Assert.Equal(
            "xcp-studio-live-adapter-description-v1",
            result.SchemaVersion);
        var commands = result.Payload.GetProperty("creative_commands")
            .EnumerateArray()
            .Select(item => item.GetString()!)
            .ToArray();
        Assert.Equal(
            [
                "describe_creative_host",
                "prepare_creative_install",
                "commit_creative_install",
                "activate_creative_install",
                "launch_creative_project",
                "observe_creative_foreground",
                "capture_creative_frame",
                "rollback_creative_activation",
                "remove_creative_install",
            ],
            commands);
        Assert.False(
            result.Payload
                .GetProperty("authentication")
                .GetProperty("persisted")
                .GetBoolean());
    }

    [Fact]
    public async Task MissingEphemeralAuthenticationIsStructuredBeforeNetwork()
    {
        var result = await CreateClient(
            inheritAuthenticationEnvironment: false)
            .DescribeCreativeHostAsync();

        Assert.False(result.Ok);
        Assert.Equal(
            "xcp.agent.auth_context_invalid",
            result.Error!.Code);
        Assert.Equal(
            "xcp-agent-error-details-v1",
            result.Error.Details!.SchemaVersion);
        Assert.Equal("auth", result.Error.Details.Stage);
        Assert.Equal(
            "XCP_PAIRING_CODE xor XCP_SESSION_ID",
            result.Error.Details.Expected);
    }

    [Fact]
    public void NavigationIsProjectFirstAndExposesAllC4BSurfaces()
    {
        Assert.Equal(
            [
                "home",
                "create",
                "projects",
                "adapt",
                "evolve",
                "playtest",
                "devices",
                "evidence",
            ],
            StudioNavigation.Items.Select(item => item.Key).ToArray());
        Assert.DoesNotContain(
            StudioNavigation.Items,
            item => item.Key is "compute" or "jobs" or "topology");
        Assert.Contains(
            "xcp_agent_lifecycle.py run-live",
            StudioNavigation.ByKey("playtest").Operations);
        Assert.DoesNotContain(
            "prepare_creative_install",
            StudioNavigation.ByKey("playtest").Operations);
        Assert.Contains(
            "xcp_source_adapt.py adapt",
            StudioNavigation.ByKey("adapt").Operations);
        Assert.Contains(
            "xcp_project_evolve.py evolve",
            StudioNavigation.ByKey("evolve").Operations);
    }

    [Fact]
    public void HostProfileProjectionKeepsDynamicIdsAndRawContract()
    {
        var result = new StudioCommandResult(
            true,
            "worker-sdk-result-v1",
            JsonSerializer.SerializeToElement(
                new
                {
                    ok = true,
                    schema_version = "worker-sdk-result-v1",
                    payload = new
                    {
                        canonical_profile_sha256 = new string('a', 64),
                        creative_host = new
                        {
                            profile_id = "xcp.creative.host.test-v1",
                            host_api_version = "0.3.0",
                            module_kinds = new[]
                            {
                                new
                                {
                                    id = "xcp.future.module.v9",
                                    admitted = true,
                                },
                                new
                                {
                                    id = "xcp.closed.module.v1",
                                    admitted = false,
                                },
                            },
                            capabilities = new[]
                            {
                                new
                                {
                                    id = "future.capability",
                                    admitted = true,
                                },
                            },
                        },
                    },
                }),
            null);

        var profile = StudioHostProfileProjection.FromWorkerResult(result);

        Assert.Equal("xcp.creative.host.test-v1", profile.ProfileId);
        Assert.Equal(["xcp.future.module.v9"], profile.ModuleKinds);
        Assert.Equal(["future.capability"], profile.Capabilities);
        Assert.Equal(
            "xcp.future.module.v9",
            profile.Raw
                .GetProperty("module_kinds")[0]
                .GetProperty("id")
                .GetString());
    }

    [Fact]
    public void SessionProjectionCannotCarrySessionOrCredentialMaterial()
    {
        var properties = typeof(StudioSessionModel)
            .GetProperties()
            .Select(property => property.Name)
            .ToArray();

        Assert.DoesNotContain(
            properties,
            name => name.Contains("Id", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(
            properties,
            name => name.Contains("Secret", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(
            properties,
            name => name.Contains("Token", StringComparison.OrdinalIgnoreCase));
    }

    private static XcpCreativeLiveCliClient CreateClient(
        bool inheritAuthenticationEnvironment = true)
    {
        var root = FindRepositoryRoot();
        return new XcpCreativeLiveCliClient(
            Path.Combine(root, "tools", "xcp_studio_live_adapter.py"),
            "127.0.0.1",
            timeoutSeconds: 1,
            inheritAuthenticationEnvironment: inheritAuthenticationEnvironment);
    }

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var candidate = Path.Combine(
                current.FullName,
                "tools",
                "xcp_studio_live_adapter.py");
            if (File.Exists(candidate))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not locate the xcompute-probe repository root.");
    }

    private sealed class FakeSessionProvider : IStudioWorkerSessionProvider
    {
        public bool Opened { get; private set; }
        public bool Disposed { get; private set; }

        public Task<IStudioWorkerSessionLease> OpenAsync(
            string deviceAddress,
            int port,
            double timeoutSeconds,
            CancellationToken cancellationToken = default)
        {
            Opened = true;
            return Task.FromResult<IStudioWorkerSessionLease>(
                new FakeLease(this));
        }

        private sealed class FakeLease(FakeSessionProvider owner)
            : IStudioWorkerSessionLease
        {
            public string SessionId => "ephemeral-test-session";

            public ValueTask DisposeAsync()
            {
                owner.Disposed = true;
                return ValueTask.CompletedTask;
            }
        }
    }
}
