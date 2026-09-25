using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json;
using XComputeControlCenter.Core;

namespace XComputeControlCenter.ViewModels;

public sealed class MainWindowViewModel : INotifyPropertyChanged
{
    private readonly IXcpCreativeProjectClient _projectClient;
    private readonly IXcpStudioWorkflowClient _workflowClient;
    private readonly IStudioProjectWorkspace _workspace;
    private readonly IStudioRecentProjectStore _recentProjects;
    private readonly Func<string, IXcpCreativeLiveClient>? _liveClientFactory;
    private readonly IStudioWorkerTrustBroker? _trustBroker;
    private IXcpCreativeLiveClient? _liveClient;
    private string _liveClientAddress = "";
    private StudioNavigationItem _selected = StudioNavigation.Items[0];
    private string _localState = "Not checked";
    private string _deviceState = "Not configured";
    private string _trustState = "First trust not configured";
    private string _deviceAddress;
    private string _detail =
        "Refresh to read the authoritative local and live contracts.";
    private string _errorCode = "";
    private string _correction = "";
    private string _structuredResult = "";
    private bool _isBusy;
    private string _projectDirectory = "";
    private string _projectId = "new-xcp-project";
    private string _projectTitle = "New XCP Project";
    private string _projectDescription =
        "An XCP creative project edited by beginner and expert views.";
    private string _projectVersion = "1.0.0";
    private string _projectSource = "";
    private string _projectSha256 = "";
    private string _bundleDirectory = "";
    private string _lifecycleBundleRoot = "";
    private string _updateProjectDirectory = "";
    private string _intentPath = "";
    private string _captureDirectory = "";
    private string _receiptPath = "";
    private string _ledgerPath = "";
    private string _runId = $"studio-{DateTime.UtcNow:yyyyMMdd-HHmmss}";
    private string _hostProfilePath = "";
    private string _externalSourceDirectory = "";
    private string _adaptationOutputDirectory = "";
    private string _priorRunDirectory = "";
    private string _overlayPath = "";
    private string _evolutionOutputDirectory = "";
    private string _originLocator = "authorized-local-tree";
    private string _revision = "working-tree";
    private string _attribution = "User-authorized local source";
    private IReadOnlyList<StudioRecentProjectEntry> _recentProjectItems = [];
    private StudioRecentProjectEntry? _selectedRecentProject;
    private readonly string _toolchainIdentity;

    public MainWindowViewModel(
        IXcpCreativeProjectClient projectClient,
        IXcpCreativeLiveClient? liveClient,
        IXcpStudioWorkflowClient workflowClient,
        IStudioProjectWorkspace workspace,
        string deviceAddress = "",
        Func<string, IXcpCreativeLiveClient>? liveClientFactory = null,
        IStudioRecentProjectStore? recentProjects = null,
        string toolchainIdentity = "portable toolchain",
        IStudioWorkerTrustBroker? trustBroker = null)
    {
        _projectClient = projectClient;
        _liveClient = liveClient;
        _workflowClient = workflowClient;
        _workspace = workspace;
        _deviceAddress = deviceAddress;
        _liveClientAddress = liveClient is null ? "" : deviceAddress;
        _liveClientFactory = liveClientFactory;
        _recentProjects =
            recentProjects ?? StudioRecentProjectStore.CreateDefault();
        _toolchainIdentity = toolchainIdentity;
        _trustBroker = trustBroker;
        if (_trustBroker?.Status is { Provisioned: true } trust)
        {
            _trustState =
                $"Trusted controller ready · {trust.PublicKeySha256[..12]}";
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public IReadOnlyList<StudioNavigationItem> NavigationItems =>
        StudioNavigation.Items;

    public StudioNavigationItem Selected
    {
        get => _selected;
        private set
        {
            if (_selected == value)
            {
                return;
            }
            _selected = value;
            RaisePropertyChanged();
            RaisePropertyChanged(nameof(SelectedOperations));
        }
    }

    public IReadOnlyList<string> SelectedOperations => Selected.Operations;

    public string LocalState
    {
        get => _localState;
        private set => SetProperty(ref _localState, value);
    }

    public string DeviceState
    {
        get => _deviceState;
        private set => SetProperty(ref _deviceState, value);
    }

    public string TrustState
    {
        get => _trustState;
        private set => SetProperty(ref _trustState, value);
    }

    public string DeviceAddress
    {
        get => _deviceAddress;
        set
        {
            if (!SetProperty(ref _deviceAddress, value))
            {
                return;
            }

            if (!string.Equals(
                    _liveClientAddress,
                    value.Trim(),
                    StringComparison.OrdinalIgnoreCase))
            {
                _liveClient = null;
                _liveClientAddress = "";
                DeviceState = string.IsNullOrWhiteSpace(value)
                    ? "Offline · enter a device address"
                    : "Not checked";
            }
        }
    }

    public string Detail
    {
        get => _detail;
        private set => SetProperty(ref _detail, value);
    }

    public string ErrorCode
    {
        get => _errorCode;
        private set => SetProperty(ref _errorCode, value);
    }

    public string Correction
    {
        get => _correction;
        private set => SetProperty(ref _correction, value);
    }

    public string StructuredResult
    {
        get => _structuredResult;
        private set => SetProperty(ref _structuredResult, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set => SetProperty(ref _isBusy, value);
    }

    public string ProjectDirectory
    {
        get => _projectDirectory;
        set => SetProperty(ref _projectDirectory, value);
    }

    public string ProjectId
    {
        get => _projectId;
        set => SetProperty(ref _projectId, value);
    }

    public string ProjectTitle
    {
        get => _projectTitle;
        set => SetProperty(ref _projectTitle, value);
    }

    public string ProjectDescription
    {
        get => _projectDescription;
        set => SetProperty(ref _projectDescription, value);
    }

    public string ProjectVersion
    {
        get => _projectVersion;
        set => SetProperty(ref _projectVersion, value);
    }

    public string ProjectSource
    {
        get => _projectSource;
        set => SetProperty(ref _projectSource, value);
    }

    public string ProjectSha256
    {
        get => _projectSha256;
        private set => SetProperty(ref _projectSha256, value);
    }

    public string BundleDirectory
    {
        get => _bundleDirectory;
        set => SetProperty(ref _bundleDirectory, value);
    }

    public string LifecycleBundleRoot
    {
        get => _lifecycleBundleRoot;
        set => SetProperty(ref _lifecycleBundleRoot, value);
    }

    public string UpdateProjectDirectory
    {
        get => _updateProjectDirectory;
        set => SetProperty(ref _updateProjectDirectory, value);
    }

    public string IntentPath
    {
        get => _intentPath;
        set => SetProperty(ref _intentPath, value);
    }

    public string CaptureDirectory
    {
        get => _captureDirectory;
        set => SetProperty(ref _captureDirectory, value);
    }

    public string ReceiptPath
    {
        get => _receiptPath;
        set => SetProperty(ref _receiptPath, value);
    }

    public string LedgerPath
    {
        get => _ledgerPath;
        set => SetProperty(ref _ledgerPath, value);
    }

    public string RunId
    {
        get => _runId;
        set => SetProperty(ref _runId, value);
    }

    public string HostProfilePath
    {
        get => _hostProfilePath;
        set => SetProperty(ref _hostProfilePath, value);
    }

    public string ExternalSourceDirectory
    {
        get => _externalSourceDirectory;
        set => SetProperty(ref _externalSourceDirectory, value);
    }

    public string AdaptationOutputDirectory
    {
        get => _adaptationOutputDirectory;
        set => SetProperty(ref _adaptationOutputDirectory, value);
    }

    public string PriorRunDirectory
    {
        get => _priorRunDirectory;
        set => SetProperty(ref _priorRunDirectory, value);
    }

    public string OverlayPath
    {
        get => _overlayPath;
        set => SetProperty(ref _overlayPath, value);
    }

    public string EvolutionOutputDirectory
    {
        get => _evolutionOutputDirectory;
        set => SetProperty(ref _evolutionOutputDirectory, value);
    }

    public string OriginLocator
    {
        get => _originLocator;
        set => SetProperty(ref _originLocator, value);
    }

    public string Revision
    {
        get => _revision;
        set => SetProperty(ref _revision, value);
    }

    public string Attribution
    {
        get => _attribution;
        set => SetProperty(ref _attribution, value);
    }

    public IReadOnlyList<StudioRecentProjectEntry> RecentProjectItems
    {
        get => _recentProjectItems;
        private set => SetProperty(ref _recentProjectItems, value);
    }

    public StudioRecentProjectEntry? SelectedRecentProject
    {
        get => _selectedRecentProject;
        set => SetProperty(ref _selectedRecentProject, value);
    }

    public string ToolchainIdentity => _toolchainIdentity;

    public void Select(string key)
    {
        Selected = StudioNavigation.ByKey(key);
    }

    public async Task RefreshAsync(CancellationToken cancellationToken = default)
    {
        await RunAsync(
            async () =>
            {
                await RefreshRecentProjectsCoreAsync(cancellationToken);
                if (string.IsNullOrWhiteSpace(ProjectDirectory) &&
                    SelectedRecentProject is
                    { Status: StudioRecentProjectStatus.Available } recent)
                {
                    ProjectDirectory = recent.ProjectDirectory;
                    await LoadProjectCoreAsync(cancellationToken);
                    SetLifecycleDefaults();
                }
                var local = await _projectClient
                    .DescribeAsync(cancellationToken)
                    .ConfigureAwait(true);
                if (!ApplyResult(local, "Local project contract ready."))
                {
                    LocalState = $"Rejected · {local.Error?.Code}";
                    return;
                }

                var lifecycle = await _workflowClient
                    .DescribeLifecycleAsync(cancellationToken)
                    .ConfigureAwait(true);
                var adaptation = await _workflowClient
                    .DescribeAdaptationAsync(cancellationToken)
                    .ConfigureAwait(true);
                var evolution = await _workflowClient
                    .DescribeEvolutionAsync(cancellationToken)
                    .ConfigureAwait(true);
                if (!lifecycle.Ok || !adaptation.Ok || !evolution.Ok)
                {
                    ApplyResult(
                        !lifecycle.Ok
                            ? lifecycle
                            : !adaptation.Ok ? adaptation : evolution,
                        "");
                    return;
                }
                LocalState =
                    $"C5 + C6 + C7 · {_toolchainIdentity} · authoritative";

                var liveClient = ResolveLiveClient();
                if (liveClient is null)
                {
                    DeviceState = "Offline · enter a device address";
                    Detail =
                        "Local create, adapt and evolve tools are ready. Device state remains explicit; no mock snapshot is substituted.";
                    return;
                }

                var adapter = await liveClient
                    .DescribeAdapterAsync(cancellationToken)
                    .ConfigureAwait(true);
                if (!ApplyResult(adapter, ""))
                {
                    return;
                }

                var discovered = await liveClient
                    .DescribeCreativeHostAsync(cancellationToken)
                    .ConfigureAwait(true);
                if (!discovered.Ok)
                {
                    DeviceState = "Unavailable";
                    ApplyResult(discovered, "");
                    return;
                }

                var profile =
                    StudioHostProfileProjection.FromWorkerResult(discovered);
                DeviceState =
                    $"{profile.ProfileId} · API {profile.HostApiVersion}";
                Detail =
                    $"{profile.ModuleKinds.Count} admitted modules · " +
                    $"{profile.Capabilities.Count} admitted capabilities · " +
                    "ephemeral SDK session";
            },
            cancellationToken);
    }

    public Task CreateProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _projectClient.CreateAsync(
                    ProjectDirectory,
                    ProjectId,
                    ProjectTitle,
                    OptionalExistingPath(HostProfilePath),
                    cancellationToken);
                if (ApplyResult(result, "Project created through the portable CLI."))
                {
                    var snapshot = await LoadProjectCoreAsync(cancellationToken);
                    SetLifecycleDefaults();
                    await RecordProjectAsync(
                        snapshot,
                        "created",
                        cancellationToken);
                    if (!await EnsureLifecycleCoreAsync(cancellationToken))
                    {
                        return;
                    }
                    Detail =
                        "Project created and ready to edit. Studio prepared its lifecycle files automatically.";
                }
            },
            cancellationToken);

    public Task OpenProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var snapshot = await LoadProjectCoreAsync(cancellationToken);
                SetLifecycleDefaults();
                await RecordProjectAsync(
                    snapshot,
                    "opened",
                    cancellationToken);
                Detail =
                    "Guided fields and expert source are bound to the same xcp-project.json revision.";
            },
            cancellationToken);

    public Task SaveGuidedAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var snapshot = await _workspace.SaveGuidedAsync(
                    ProjectDirectory,
                    ProjectSha256,
                    ProjectTitle,
                    ProjectDescription,
                    ProjectVersion,
                    cancellationToken);
                ApplySnapshot(snapshot);
                await RecordProjectAsync(
                    snapshot,
                    "edited",
                    cancellationToken);
                Detail =
                    "Guided changes atomically replaced the same xcp-project.json read by Expert mode.";
            },
            cancellationToken);

    public Task SaveSourceAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var snapshot = await _workspace.SaveSourceAsync(
                    ProjectDirectory,
                    ProjectSha256,
                    ProjectSource,
                    cancellationToken);
                ApplySnapshot(snapshot);
                await RecordProjectAsync(
                    snapshot,
                    "edited",
                    cancellationToken);
                Detail =
                    "Expert source atomically replaced the same xcp-project.json read by Guided mode.";
            },
            cancellationToken);

    public Task ValidateProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunProjectCommandAsync(
            () => _projectClient.ValidateAsync(
                ProjectDirectory,
                OptionalExistingPath(HostProfilePath),
                cancellationToken),
            "Portable validation passed.",
            cancellationToken);

    public Task BuildProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunProjectCommandAsync(
            () => _projectClient.BuildAsync(
                ProjectDirectory,
                BundleDirectory,
                OptionalExistingPath(HostProfilePath),
                cancellationToken),
            "Deterministic bundle built through the portable CLI.",
            cancellationToken);

    public Task PrepareProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var validation = await _projectClient.ValidateAsync(
                    ProjectDirectory,
                    OptionalExistingPath(HostProfilePath),
                    cancellationToken);
                if (!ApplyResult(validation, ""))
                {
                    return;
                }
                var build = await _projectClient.BuildAsync(
                    ProjectDirectory,
                    BundleDirectory,
                    OptionalExistingPath(HostProfilePath),
                    cancellationToken);
                ApplyResult(
                    build,
                    "Project validated and its deterministic Xbox bundle is ready.");
            },
            cancellationToken);

    public Task InitializeLifecycleAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            () => EnsureLifecycleCoreAsync(cancellationToken),
            cancellationToken);

    public Task PlayOnXboxAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var snapshot = await LoadProjectCoreAsync(cancellationToken);
                SetLifecycleDefaults();
                ConfigureFreshPreviewRun();
                if (!await EnsureLifecycleCoreAsync(cancellationToken))
                {
                    return;
                }

                Detail = "Preparing, sending and starting your project…";
                var result = await _workflowClient.RunPreviewAsync(
                    new StudioPreviewRequest(
                        DeviceAddress.Trim(),
                        ProjectDirectory,
                        LifecycleBundleRoot,
                        IntentPath,
                        CaptureDirectory,
                        ReceiptPath,
                        LedgerPath,
                        RunId),
                    cancellationToken);
                if (ApplyResult(
                        result,
                        "Playing on Xbox. Studio validated the project and preserved the run evidence under Details."))
                {
                    await RecordProjectAsync(
                        snapshot,
                        "played",
                        cancellationToken);
                }
            },
            cancellationToken);

    public Task RunLifecycleAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                if (!await EnsureLifecycleCoreAsync(cancellationToken))
                {
                    return;
                }
                var result = await _workflowClient.RunLifecycleAsync(
                    new StudioLifecycleRequest(
                        DeviceAddress.Trim(),
                        ProjectDirectory,
                        UpdateProjectDirectory,
                        LifecycleBundleRoot,
                        IntentPath,
                        CaptureDirectory,
                        ReceiptPath,
                        LedgerPath,
                        RunId,
                        ProveStructuredLiveCorrection: true),
                    cancellationToken);
                ApplyResult(
                    result,
                    "C5 completed install, activate, launch, observe, capture, update, rollback and cleanup.");
            },
            cancellationToken);

    public Task DetectSourceAsync(
        CancellationToken cancellationToken = default) =>
        RunProjectCommandAsync(
            () => _workflowClient.DetectSourceAsync(
                ExternalSourceDirectory,
                cancellationToken),
            "C6 detected the external ecosystem through its versioned adapter.",
            cancellationToken);

    public Task DiscoverHostProfileAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _workflowClient.DiscoverHostAsync(
                    DeviceAddress.Trim(),
                    HostProfilePath,
                    cancellationToken: cancellationToken);
                ApplyResult(
                    result,
                    "C5 discovered and saved the exact Creative Host profile for C6/C7 admission.");
            },
            cancellationToken);

    public Task EnrollTrustedControllerAsync(
        string pairingCode,
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                if (_trustBroker is null)
                {
                    throw new StudioTrustException(
                        "xcp.studio.trust_broker_unavailable",
                        "Trusted-controller enrollment is unavailable.",
                        "trust_broker",
                        "Windows user key-store broker",
                        "missing",
                        "run the packaged Windows x64 Studio build");
                }
                var enrollment = await _trustBroker.EnrollAsync(
                    DeviceAddress.Trim(),
                    pairingCode,
                    cancellationToken: cancellationToken)
                    .ConfigureAwait(true);
                TrustState =
                    $"Trusted controller registered · {enrollment.PublicKeySha256[..12]}";
                DeviceState = "Trusted · ready to connect without the PIN";
                StructuredResult = JsonSerializer.Serialize(
                    new
                    {
                        ok = true,
                        schema_version = "xcp-studio-trust-enrollment-v1",
                        controller_id = enrollment.ControllerId,
                        public_key_sha256 = enrollment.PublicKeySha256,
                        trusted_controller_count = enrollment.TrustedControllerCount,
                        authentication_method = enrollment.AuthenticationMethod,
                        pairing_code_stored = false,
                        private_key_exported = false,
                    },
                    new JsonSerializerOptions { WriteIndented = true });
                Detail =
                    "First trust completed. Later operations open signed temporary sessions; the pairing code was not persisted.";
            },
            cancellationToken);

    public Task AdaptSourceAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _workflowClient.AdaptSourceAsync(
                    new StudioAdaptationRequest(
                        ExternalSourceDirectory,
                        AdaptationOutputDirectory,
                        HostProfilePath,
                        ProjectTitle,
                        "authorized_local_tree",
                        OriginLocator,
                        Revision,
                        "user_attested_authorization",
                        "verified",
                        Attribution),
                    cancellationToken);
                if (ApplyResult(
                        result,
                        "C6 emitted Creative IR, adaptation plan and an ordinary C5 handoff."))
                {
                    BindLifecycleHandoff(
                        AdaptationOutputDirectory,
                        Path.Combine(
                            AdaptationOutputDirectory,
                            "xcp-project"),
                        Path.Combine(
                            AdaptationOutputDirectory,
                            "xcp-project"));
                    var snapshot = await LoadProjectCoreAsync(
                        cancellationToken);
                    await RecordProjectAsync(
                        snapshot,
                        "adapted",
                        cancellationToken);
                }
            },
            cancellationToken);

    public Task EvolveProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _workflowClient.EvolveProjectAsync(
                    new StudioEvolutionRequest(
                        PriorRunDirectory,
                        ExternalSourceDirectory,
                        OverlayPath,
                        EvolutionOutputDirectory,
                        HostProfilePath,
                        ProjectTitle,
                        "authorized_local_tree",
                        OriginLocator,
                        Revision,
                        "user_attested_authorization",
                        "verified",
                        Attribution),
                    cancellationToken);
                if (ApplyResult(
                        result,
                        "C7 emitted the reconciled project and ordinary C5 handoff."))
                {
                    BindLifecycleHandoff(
                        EvolutionOutputDirectory,
                        Path.Combine(PriorRunDirectory, "xcp-project"),
                        Path.Combine(
                            EvolutionOutputDirectory,
                            "xcp-project"));
                    var snapshot = await LoadProjectCoreAsync(
                        cancellationToken);
                    await RecordProjectAsync(
                        snapshot,
                        "evolved",
                        cancellationToken);
                }
            },
            cancellationToken);

    public Task RefreshRecentProjectsAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                await RefreshRecentProjectsCoreAsync(cancellationToken);
                Detail =
                    $"{RecentProjectItems.Count} recent project record(s); availability was checked against xcp-project.json.";
            },
            cancellationToken);

    public Task OpenSelectedRecentProjectAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                if (SelectedRecentProject is null)
                {
                    throw RecentSelectionRequired();
                }
                ProjectDirectory = SelectedRecentProject.ProjectDirectory;
                var snapshot = await LoadProjectCoreAsync(cancellationToken);
                SetLifecycleDefaults();
                await RecordProjectAsync(
                    snapshot,
                    "opened",
                    cancellationToken);
                Detail =
                    "The selected recent project is open from its canonical xcp-project.json.";
            },
            cancellationToken);

    public Task RelinkSelectedRecentProjectAsync(
        string relocatedDirectory,
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                if (SelectedRecentProject is null)
                {
                    throw RecentSelectionRequired();
                }
                var selectedOriginal = SelectedRecentProject.OriginalDirectory;
                var snapshot = await _workspace.OpenAsync(
                    relocatedDirectory,
                    cancellationToken);
                RecentProjectItems = await _recentProjects.RelinkAsync(
                    selectedOriginal,
                    snapshot,
                    cancellationToken);
                SelectedRecentProject = RecentProjectItems.First(entry =>
                    string.Equals(
                        entry.ProjectDirectory,
                        snapshot.ProjectDirectory,
                        StringComparison.OrdinalIgnoreCase));
                ApplySnapshot(snapshot);
                SetLifecycleDefaults();
                Detail =
                    "The project was relinked explicitly; its original and current locations remain visible.";
            },
            cancellationToken);

    public Task FinalizeAdaptationAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _workflowClient.FinalizeAdaptationAsync(
                    new StudioFinalizationRequest(
                        AdaptationOutputDirectory,
                        ReceiptPath,
                        LedgerPath,
                        Path.Combine(
                            AdaptationOutputDirectory,
                            "readiness-final.json")),
                    cancellationToken);
                ApplyResult(
                    result,
                    "C6 fidelity/readiness report finalized from the exact C5 receipt and ledger.");
            },
            cancellationToken);

    public Task FinalizeEvolutionAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(
            async () =>
            {
                var result = await _workflowClient.FinalizeEvolutionAsync(
                    new StudioFinalizationRequest(
                        EvolutionOutputDirectory,
                        ReceiptPath,
                        LedgerPath,
                        Path.Combine(
                            EvolutionOutputDirectory,
                            "evolution-final.json")),
                    cancellationToken);
                ApplyResult(
                    result,
                    "C7 evolution report finalized from the exact C5 receipt and ledger.");
            },
            cancellationToken);

    private Task RunProjectCommandAsync(
        Func<Task<StudioCommandResult>> operation,
        string successDetail,
        CancellationToken cancellationToken) =>
        RunAsync(
            async () =>
            {
                var result = await operation();
                ApplyResult(result, successDetail);
            },
            cancellationToken);

    private async Task<bool> EnsureLifecycleCoreAsync(
        CancellationToken cancellationToken)
    {
        if (!File.Exists(IntentPath))
        {
            var intent = await _workflowClient.InitIntentAsync(
                new StudioIntentRequest(
                    IntentPath,
                    ProjectId,
                    ProjectDescription,
                    "other"),
                cancellationToken);
            if (!ApplyResult(intent, ""))
            {
                return false;
            }
        }
        if (!File.Exists(LedgerPath))
        {
            var ledger = await _workflowClient.InitLedgerAsync(
                LedgerPath,
                ProjectId,
                IntentPath,
                cancellationToken);
            return ApplyResult(
                ledger,
                "Lifecycle files initialized.");
        }
        Detail = "Lifecycle files are ready.";
        return true;
    }

    private async Task RunAsync(
        Func<Task> operation,
        CancellationToken cancellationToken)
    {
        if (IsBusy)
        {
            return;
        }

        IsBusy = true;
        ErrorCode = "";
        Correction = "";
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            await operation();
        }
        catch (StudioWorkspaceException exception)
        {
            ErrorCode = exception.Code;
            Correction = exception.Details.Correction;
            StructuredResult = JsonSerializer.Serialize(
                new
                {
                    ok = false,
                    error = new
                    {
                        code = exception.Code,
                        message = exception.Message,
                        details = exception.Details,
                    },
                },
                new JsonSerializerOptions { WriteIndented = true });
            Detail =
                $"{exception.Details.Stage} · {exception.Details.Path} · expected {exception.Details.Expected} · actual {exception.Details.Actual}";
        }
        catch (StudioRecentProjectStoreException exception)
        {
            ErrorCode = exception.Code;
            Correction = exception.Details.Correction;
            StructuredResult = JsonSerializer.Serialize(
                new
                {
                    ok = false,
                    error = new
                    {
                        code = exception.Code,
                        message = exception.Message,
                        details = exception.Details,
                    },
                },
                new JsonSerializerOptions { WriteIndented = true });
            Detail =
                $"{exception.Details.Stage} · {exception.Details.Path} · expected {exception.Details.Expected} · actual {exception.Details.Actual}";
        }
        catch (StudioTrustException exception)
        {
            ErrorCode = exception.Code;
            Correction = exception.Details.Correction;
            StructuredResult = JsonSerializer.Serialize(
                new
                {
                    ok = false,
                    error = new
                    {
                        code = exception.Code,
                        message = exception.Message,
                        details = exception.Details,
                    },
                },
                new JsonSerializerOptions { WriteIndented = true });
            Detail =
                $"{exception.Details.Stage} · {exception.Details.Field} · expected {exception.Details.Expected} · actual {exception.Details.Actual}";
        }
        catch (Exception exception)
        {
            ErrorCode = "xcp.studio.client_exception";
            Correction = exception.Message;
            Detail =
                "Studio failed closed before projecting unstructured state.";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool ApplyResult(StudioCommandResult result, string successDetail)
    {
        StructuredResult = result.Payload.GetRawText();
        if (result.Ok)
        {
            ErrorCode = "";
            Correction = "";
            if (!string.IsNullOrEmpty(successDetail))
            {
                Detail = successDetail;
            }
            return true;
        }

        ErrorCode = result.Error?.Code ?? "xcp.studio.unknown";
        Correction =
            result.Error?.Details?.Correction ??
            "Inspect the structured error details.";
        Detail = result.Error?.Message ??
            "Studio could not complete this action.";
        return false;
    }

    private async Task<StudioProjectSnapshot> LoadProjectCoreAsync(
        CancellationToken cancellationToken)
    {
        var snapshot = await _workspace.OpenAsync(
            ProjectDirectory,
            cancellationToken);
        ApplySnapshot(snapshot);
        return snapshot;
    }

    private async Task RecordProjectAsync(
        StudioProjectSnapshot snapshot,
        string provenance,
        CancellationToken cancellationToken)
    {
        RecentProjectItems = await _recentProjects.RecordAsync(
            snapshot,
            provenance,
            cancellationToken);
        SelectedRecentProject = RecentProjectItems.FirstOrDefault(entry =>
            string.Equals(
                entry.ProjectDirectory,
                snapshot.ProjectDirectory,
                StringComparison.OrdinalIgnoreCase));
    }

    private async Task RefreshRecentProjectsCoreAsync(
        CancellationToken cancellationToken)
    {
        var selectedOriginal = SelectedRecentProject?.OriginalDirectory;
        RecentProjectItems = await _recentProjects.LoadAsync(cancellationToken);
        SelectedRecentProject = string.IsNullOrWhiteSpace(selectedOriginal)
            ? RecentProjectItems.FirstOrDefault()
            : RecentProjectItems.FirstOrDefault(entry =>
                string.Equals(
                    entry.OriginalDirectory,
                    selectedOriginal,
                    StringComparison.OrdinalIgnoreCase));
    }

    private static StudioRecentProjectStoreException RecentSelectionRequired() =>
        new(
            "xcp.studio.recent_project_selection_required",
            "No recent project is selected.",
            "recent-project-library",
            "one selected recent project",
            "none",
            "Select a project record and retry.");

    private void ApplySnapshot(StudioProjectSnapshot snapshot)
    {
        ProjectDirectory = snapshot.ProjectDirectory;
        ProjectId = snapshot.ProjectId;
        ProjectVersion = snapshot.Version;
        ProjectTitle = snapshot.Title;
        ProjectDescription = snapshot.Description;
        ProjectSource = snapshot.SourceText;
        ProjectSha256 = snapshot.FileSha256;
        LocalState =
            $"{snapshot.ProjectId}@{snapshot.Version} · {snapshot.FileSha256[..12]}";
    }

    private void SetLifecycleDefaults()
    {
        if (string.IsNullOrWhiteSpace(BundleDirectory))
        {
            BundleDirectory =
                Path.Combine(ProjectDirectory, ".xcp", "local-bundle");
        }
        if (string.IsNullOrWhiteSpace(LifecycleBundleRoot))
        {
            LifecycleBundleRoot =
                Path.Combine(ProjectDirectory, ".xcp", $"{RunId}-bundles");
        }
        if (string.IsNullOrWhiteSpace(UpdateProjectDirectory))
        {
            UpdateProjectDirectory = ProjectDirectory;
        }
        if (string.IsNullOrWhiteSpace(IntentPath))
        {
            IntentPath = Path.Combine(ProjectDirectory, ".xcp", "intent.json");
        }
        if (string.IsNullOrWhiteSpace(HostProfilePath))
        {
            HostProfilePath =
                Path.Combine(ProjectDirectory, ".xcp", "host-profile.json");
        }
        if (string.IsNullOrWhiteSpace(CaptureDirectory))
        {
            CaptureDirectory = Path.Combine(ProjectDirectory, ".xcp", "captures");
        }
        if (string.IsNullOrWhiteSpace(ReceiptPath))
        {
            ReceiptPath = Path.Combine(ProjectDirectory, ".xcp", "receipt.json");
        }
        if (string.IsNullOrWhiteSpace(LedgerPath))
        {
            LedgerPath = Path.Combine(ProjectDirectory, ".xcp", "ledger.json");
        }
    }

    private void BindLifecycleHandoff(
        string runRoot,
        string baseProjectDirectory,
        string updateProjectDirectory)
    {
        ProjectDirectory = baseProjectDirectory;
        UpdateProjectDirectory = updateProjectDirectory;
        IntentPath = Path.Combine(runRoot, "c5-intent.json");
        LedgerPath = Path.Combine(runRoot, "c5-correction-ledger.json");
        ReceiptPath = Path.Combine(runRoot, "c5-live-receipt.json");
        CaptureDirectory = Path.Combine(runRoot, $"{RunId}-captures");
        LifecycleBundleRoot = Path.Combine(runRoot, $"{RunId}-bundles");
    }

    private void ConfigureFreshPreviewRun()
    {
        RunId =
            $"studio-preview-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}"[..38];
        IntentPath = Path.Combine(ProjectDirectory, ".xcp", "intent.json");
        LedgerPath = Path.Combine(ProjectDirectory, ".xcp", "ledger.json");
        var runRoot = Path.Combine(
            Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData),
            "XCP",
            "Studio",
            "runs",
            ProjectId,
            RunId);
        LifecycleBundleRoot = Path.Combine(runRoot, "bundles");
        CaptureDirectory = Path.Combine(runRoot, "captures");
        ReceiptPath = Path.Combine(runRoot, "receipt.json");
    }

    private static string? OptionalPath(string value) =>
        string.IsNullOrWhiteSpace(value) ? null : value;

    private static string? OptionalExistingPath(string value) =>
        string.IsNullOrWhiteSpace(value) || !File.Exists(value) ? null : value;

    private void RaisePropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    private bool SetProperty<T>(
        ref T field,
        T value,
        [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }
        field = value;
        RaisePropertyChanged(name);
        return true;
    }

    private IXcpCreativeLiveClient? ResolveLiveClient()
    {
        var address = DeviceAddress.Trim();
        if (string.IsNullOrWhiteSpace(address))
        {
            return null;
        }

        if (_liveClient is not null &&
            string.Equals(
                _liveClientAddress,
                address,
                StringComparison.OrdinalIgnoreCase))
        {
            return _liveClient;
        }

        if (_liveClientFactory is null)
        {
            return null;
        }

        _liveClient = _liveClientFactory(address);
        _liveClientAddress = address;
        return _liveClient;
    }
}
