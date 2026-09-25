using XComputeControlCenter.Core;
using Xunit;

namespace XComputeControlCenter.Core.Tests;

public sealed class XcpStudioWorkflowCliClientTests
{
    [Fact]
    public async Task IsolatedLauncherDoesNotMutateManifestBoundToolDirectory()
    {
        var root = NewTemporaryRoot();
        try
        {
            var helper = Path.Combine(root, "manifest_bound_helper.py");
            var tool = Path.Combine(root, "workflow.py");
            await File.WriteAllTextAsync(helper, "VALUE = 7\n");
            await File.WriteAllTextAsync(
                tool,
                "import json, manifest_bound_helper\n" +
                "print(json.dumps({'ok': True, 'schema_version': 'xcp-test-v1', 'value': manifest_bound_helper.VALUE}))\n");
            var client = new XcpStudioWorkflowCliClient(tool, tool, tool);

            var result = await client.DescribeLifecycleAsync();

            Assert.True(result.Ok);
            Assert.Equal(7, result.Payload.GetProperty("value").GetInt32());
            Assert.False(Directory.Exists(Path.Combine(root, "__pycache__")));
            Assert.Empty(Directory.EnumerateFiles(
                root,
                "*.pyc",
                SearchOption.AllDirectories));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task LiveWorkflowUsesAndClosesTrustedSessionLease()
    {
        var root = NewTemporaryRoot();
        try
        {
            var tool = Path.Combine(root, "workflow.py");
            await File.WriteAllTextAsync(
                tool,
                "import json, os\n" +
                "assert os.environ.get('XCP_SESSION_ID') == 'workflow-test-session'\n" +
                "assert not os.environ.get('XCP_PAIRING_CODE')\n" +
                "print(json.dumps({'ok': True, 'schema_version': 'xcp-agent-host-profile-written-v1'}))\n");
            var provider = new FakeSessionProvider();
            var client = new XcpStudioWorkflowCliClient(
                tool,
                tool,
                tool,
                inheritAuthenticationEnvironment: false,
                sessionProvider: provider);

            var result = await client.DiscoverHostAsync(
                "127.0.0.1",
                Path.Combine(root, "profile.json"),
                timeoutSeconds: 1);

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
    public async Task DescriptionsBindC5C6AndC7WithoutPrivateLifecycle()
    {
        var client = CreateClient();
        var lifecycle = await client.DescribeLifecycleAsync();
        var adaptation = await client.DescribeAdaptationAsync();
        var evolution = await client.DescribeEvolutionAsync();

        Assert.True(lifecycle.Ok);
        Assert.Equal(
            "C5_AGENT_NATIVE_CREATION_GATE",
            lifecycle.Payload.GetProperty("gate_id").GetString());
        Assert.Contains(
            "run-preview",
            lifecycle.Payload.GetProperty("commands")
                .EnumerateArray()
                .Select(item => item.GetString()));
        Assert.Contains(
            "run-live",
            lifecycle.Payload.GetProperty("commands")
                .EnumerateArray()
                .Select(item => item.GetString()));

        Assert.True(adaptation.Ok);
        Assert.Equal(
            "C6_AGENT_NATIVE_SOURCE_ADAPTATION_V1",
            adaptation.Payload.GetProperty("gate_id").GetString());
        Assert.Equal(
            "xcp.creative_ir_to_project.v1",
            adaptation.Payload.GetProperty("backend")
                .GetProperty("backend_id")
                .GetString());

        Assert.True(evolution.Ok);
        Assert.Equal(
            "C7_PROJECT_EVOLUTION_AND_CONTINUOUS_READAPTATION",
            evolution.Payload.GetProperty("gate_id").GetString());
        Assert.Equal(
            "C5_AGENT_NATIVE_CREATION_GATE",
            evolution.Payload.GetProperty("lifecycle_owner").GetString());
    }

    [Fact]
    public async Task PreviewFailurePreservesStructuredAuthorityBeforeNetwork()
    {
        var root = NewTemporaryRoot();
        try
        {
            var result = await CreateClient(
                inheritAuthenticationEnvironment: false)
                .RunPreviewAsync(
                    new StudioPreviewRequest(
                        "127.0.0.1",
                        Path.Combine(root, "project"),
                        Path.Combine(root, "bundles"),
                        Path.Combine(root, "intent.json"),
                        Path.Combine(root, "captures"),
                        Path.Combine(root, "receipt.json"),
                        Path.Combine(root, "ledger.json"),
                        "studio-preview-structured-negative",
                        TimeoutSeconds: 1));

            Assert.False(result.Ok);
            Assert.Equal("xcp.agent.auth_context_invalid", result.Error!.Code);
            Assert.Equal("auth", result.Error.Details!.Stage);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task C5LiveFailurePreservesStructuredCodeAndDetailsBeforeNetwork()
    {
        var root = NewTemporaryRoot();
        try
        {
            var result = await CreateClient(
                inheritAuthenticationEnvironment: false)
                .RunLifecycleAsync(
                    new StudioLifecycleRequest(
                        "127.0.0.1",
                        Path.Combine(root, "base"),
                        Path.Combine(root, "update"),
                        Path.Combine(root, "bundles"),
                        Path.Combine(root, "intent.json"),
                        Path.Combine(root, "captures"),
                        Path.Combine(root, "receipt.json"),
                        Path.Combine(root, "ledger.json"),
                        "studio-structured-live-negative",
                        TimeoutSeconds: 1));

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
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task C5IntentAndLedgerRemainTheOnlyLifecyclePreparationAuthority()
    {
        var root = NewTemporaryRoot();
        try
        {
            var client = CreateClient();
            var intentPath = Path.Combine(root, "intent.json");
            var ledgerPath = Path.Combine(root, "ledger.json");

            var intent = await client.InitIntentAsync(
                new StudioIntentRequest(
                    intentPath,
                    "studio-authority",
                    "Studio authority test.",
                    "application"));
            Assert.True(intent.Ok);
            Assert.Equal(
                "xcp-agent-intent-created-v1",
                intent.SchemaVersion);

            var ledger = await client.InitLedgerAsync(
                ledgerPath,
                "studio-authority",
                intentPath);
            Assert.True(ledger.Ok);
            Assert.Equal(
                "xcp-agent-correction-ledger-created-v1",
                ledger.SchemaVersion);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static XcpStudioWorkflowCliClient CreateClient(
        bool inheritAuthenticationEnvironment = true)
    {
        var root = FindRepositoryRoot();
        return new XcpStudioWorkflowCliClient(
            Path.Combine(root, "tools", "xcp_agent_lifecycle.py"),
            Path.Combine(root, "tools", "xcp_source_adapt.py"),
            Path.Combine(root, "tools", "xcp_project_evolve.py"),
            inheritAuthenticationEnvironment: inheritAuthenticationEnvironment);
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
                        "xcp_agent_lifecycle.py")))
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
            $"xcp-studio-workflow-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
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
            public string SessionId => "workflow-test-session";

            public ValueTask DisposeAsync()
            {
                owner.Disposed = true;
                return ValueTask.CompletedTask;
            }
        }
    }
}
