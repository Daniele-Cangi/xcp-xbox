using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class EvolvePage : Page
{
    private readonly Action<string> _navigate;
    private readonly IStudioPathPicker _picker;

    public MainWindowViewModel ViewModel { get; }

    public EvolvePage(
        MainWindowViewModel viewModel,
        Action<string> navigate,
        IStudioPathPicker picker)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        _picker = picker;
        InitializeComponent();
    }

    private async void PickPriorRun_Click(object sender, RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.PriorRunDirectory = path;
        }
    }

    private async void PickSource_Click(object sender, RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.ExternalSourceDirectory = path;
        }
    }

    private async void PickOverlay_Click(object sender, RoutedEventArgs e)
    {
        var path = await _picker.PickJsonFileAsync();
        if (path is not null)
        {
            ViewModel.OverlayPath = path;
        }
    }

    private async void PickOutputParent_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.EvolutionOutputDirectory = Path.Combine(
                path,
                $"{ViewModel.ProjectId}-evolution");
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

    private async void EvolveProject_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.EvolveProjectAsync();

    private async void FinalizeEvolution_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.FinalizeEvolutionAsync();

    private void Playtest_Click(object sender, RoutedEventArgs e) =>
        _navigate("playtest");
}
