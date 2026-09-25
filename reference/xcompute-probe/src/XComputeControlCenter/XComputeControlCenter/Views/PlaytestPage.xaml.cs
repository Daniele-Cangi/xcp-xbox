using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class PlaytestPage : Page
{
    private readonly Action<string> _navigate;
    private readonly IStudioPathPicker _picker;

    public MainWindowViewModel ViewModel { get; }

    public PlaytestPage(
        MainWindowViewModel viewModel,
        Action<string> navigate,
        IStudioPathPicker picker)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        _picker = picker;
        InitializeComponent();
    }

    private async void PickBaseProject_Click(object sender, RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.ProjectDirectory = path;
        }
    }

    private async void PickUpdateProject_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.UpdateProjectDirectory = path;
        }
    }

    private async void PickRunOutputParent_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.LifecycleBundleRoot = Path.Combine(
                path,
                $"{ViewModel.RunId}-bundles");
            ViewModel.CaptureDirectory = Path.Combine(
                path,
                $"{ViewModel.RunId}-captures");
            ViewModel.IntentPath = Path.Combine(
                path,
                $"{ViewModel.RunId}-intent.json");
            ViewModel.LedgerPath = Path.Combine(
                path,
                $"{ViewModel.RunId}-ledger.json");
            ViewModel.ReceiptPath = Path.Combine(
                path,
                $"{ViewModel.RunId}-receipt.json");
        }
    }

    private async void InitializeLifecycle_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.InitializeLifecycleAsync();

    private async void PlayPreview_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.PlayOnXboxAsync();

    private async void RunLifecycle_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.RunLifecycleAsync();

    private void Device_Click(object sender, RoutedEventArgs e) =>
        _navigate("devices");

    private void Evidence_Click(object sender, RoutedEventArgs e) =>
        _navigate("evidence");
}
