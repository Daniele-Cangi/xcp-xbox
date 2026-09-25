using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.Core;
using XComputeControlCenter.ViewModels;
using XComputeControlCenter.Views;

namespace XComputeControlCenter;

public sealed partial class MainWindow : Window
{
    private readonly IReadOnlyDictionary<string, Page> _pages;

    public MainWindowViewModel ViewModel { get; }

    public MainWindow()
    {
        var toolchain = StudioToolchainResolver.Resolve(
            AppContext.BaseDirectory);
        var pythonRuntime = StudioPythonRuntimeResolver.Resolve(
            AppContext.BaseDirectory);
        var projectToolPath = toolchain.Entrypoint("project_builder");
        var lifecycleToolPath = toolchain.Entrypoint("lifecycle");
        var adaptationToolPath = toolchain.Entrypoint("source_adaptation");
        var evolutionToolPath = toolchain.Entrypoint("project_evolution");
        var liveAdapterPath = toolchain.Entrypoint("studio_live_adapter");
        RequireToolName(projectToolPath, "xcp_creative_project.py");
        RequireToolName(lifecycleToolPath, "xcp_agent_lifecycle.py");
        RequireToolName(adaptationToolPath, "xcp_source_adapt.py");
        RequireToolName(evolutionToolPath, "xcp_project_evolve.py");
        RequireToolName(liveAdapterPath, "xcp_studio_live_adapter.py");
        var projectClient = new XcpCreativeCliClient(
            projectToolPath,
            pythonRuntime.Executable);
        var trustBroker = StudioTrustedControllerBroker.CreateDefault();
        var address =
            Environment.GetEnvironmentVariable("XCP_DEVICE_ADDRESS") ?? "";
        IXcpCreativeLiveClient? liveClient =
            string.IsNullOrWhiteSpace(address)
                ? null
                : new XcpCreativeLiveCliClient(
                    liveAdapterPath,
                    address,
                    pythonExecutable: pythonRuntime.Executable,
                    inheritAuthenticationEnvironment: false,
                    sessionProvider: trustBroker);
        var workflowClient = new XcpStudioWorkflowCliClient(
            lifecycleToolPath,
            adaptationToolPath,
            evolutionToolPath,
            pythonRuntime.Executable,
            inheritAuthenticationEnvironment: false,
            sessionProvider: trustBroker);

        ViewModel = new MainWindowViewModel(
            projectClient,
            liveClient,
            workflowClient,
            new StudioProjectWorkspace(),
            address,
            deviceAddress => new XcpCreativeLiveCliClient(
                liveAdapterPath,
                deviceAddress,
                pythonExecutable: pythonRuntime.Executable,
                inheritAuthenticationEnvironment: false,
                sessionProvider: trustBroker),
            StudioRecentProjectStore.CreateDefault(),
            $"kit {toolchain.KitVersion} · {toolchain.ManifestSha256[..12]} · Python {pythonRuntime.RuntimeVersion} · {pythonRuntime.ManifestSha256[..12]}",
            trustBroker);

        InitializeComponent();
        var picker = new StudioPathPicker(this);
        _pages = new Dictionary<string, Page>(StringComparer.OrdinalIgnoreCase)
        {
            ["home"] = new HomePage(ViewModel, NavigateTo),
            ["create"] = new CreatePage(ViewModel, NavigateTo, picker),
            ["projects"] = new ProjectsPage(ViewModel, NavigateTo, picker),
            ["adapt"] = new AdaptPage(ViewModel, NavigateTo, picker),
            ["evolve"] = new EvolvePage(ViewModel, NavigateTo, picker),
            ["playtest"] = new PlaytestPage(ViewModel, NavigateTo, picker),
            ["devices"] = new DevicesPage(ViewModel),
            ["evidence"] = new EvidencePage(ViewModel),
        };
        ShellNavigation.SelectedItem = HomeItem;
        ShowPage("home");
    }

    private async void ShellRoot_Loaded(object sender, RoutedEventArgs e)
    {
        await ViewModel.RefreshAsync();
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e)
    {
        await ViewModel.RefreshAsync();
    }

    private void ShellNavigation_SelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item &&
            item.Tag is string key)
        {
            ShowPage(key);
        }
    }

    private void NavigateTo(string key)
    {
        var target = ShellNavigation.MenuItems
            .Concat(ShellNavigation.FooterMenuItems)
            .OfType<NavigationViewItem>()
            .FirstOrDefault(
                item => string.Equals(
                    item.Tag as string,
                    key,
                    StringComparison.OrdinalIgnoreCase));
        if (target is not null)
        {
            ShellNavigation.SelectedItem = target;
        }
        else
        {
            ShowPage(key);
        }
    }

    private void ShowPage(string key)
    {
        ViewModel.Select(key);
        ContentFrame.Content = _pages[key];
    }

    private static void RequireToolName(string path, string expected)
    {
        if (!string.Equals(
                Path.GetFileName(path),
                expected,
                StringComparison.Ordinal))
        {
            throw new StudioToolchainException(
                "xcp.studio.toolchain_entrypoint_invalid",
                $"The manifest published an unexpected Studio tool for {expected}.",
                path,
                expected,
                Path.GetFileName(path),
                "Reject the kit and export exact canonical bytes.");
        }
    }
}
