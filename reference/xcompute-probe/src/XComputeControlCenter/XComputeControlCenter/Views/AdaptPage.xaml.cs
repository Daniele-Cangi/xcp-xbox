using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class AdaptPage : Page
{
    private readonly Action<string> _navigate;
    private readonly IStudioPathPicker _picker;

    public MainWindowViewModel ViewModel { get; }

    public AdaptPage(
        MainWindowViewModel viewModel,
        Action<string> navigate,
        IStudioPathPicker picker)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        _picker = picker;
        InitializeComponent();
    }

    private async void PickSource_Click(object sender, RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.ExternalSourceDirectory = path;
        }
    }

    private async void PickOutputParent_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.AdaptationOutputDirectory = Path.Combine(
                path,
                $"{ViewModel.ProjectId}-adaptation");
        }
    }

    private async void PickHostProfile_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickJsonFileAsync();
        if (path is not null)
        {
            ViewModel.HostProfilePath = path;
        }
    }

    private async void DetectSource_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.DetectSourceAsync();

    private async void AdaptSource_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.AdaptSourceAsync();

    private async void FinalizeAdaptation_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.FinalizeAdaptationAsync();

    private void Workspace_Click(object sender, RoutedEventArgs e) =>
        _navigate("projects");

    private void Playtest_Click(object sender, RoutedEventArgs e) =>
        _navigate("playtest");
}
