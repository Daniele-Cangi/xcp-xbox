using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class ProjectsPage : Page
{
    private readonly Action<string> _navigate;
    private readonly IStudioPathPicker _picker;

    public MainWindowViewModel ViewModel { get; }

    public ProjectsPage(
        MainWindowViewModel viewModel,
        Action<string> navigate,
        IStudioPathPicker picker)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        _picker = picker;
        InitializeComponent();
    }

    private async void BrowseProject_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            ViewModel.ProjectDirectory = path;
        }
    }

    private async void RefreshLibrary_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.RefreshRecentProjectsAsync();

    private async void OpenRecent_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.OpenSelectedRecentProjectAsync();

    private async void RelinkRecent_Click(
        object sender,
        RoutedEventArgs e)
    {
        var path = await _picker.PickFolderAsync();
        if (path is not null)
        {
            await ViewModel.RelinkSelectedRecentProjectAsync(path);
        }
    }

    private async void OpenProject_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.OpenProjectAsync();

    private async void SaveGuided_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.SaveGuidedAsync();

    private async void SaveSource_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.SaveSourceAsync();

    private async void ValidateProject_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.ValidateProjectAsync();

    private async void BuildProject_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.BuildProjectAsync();

    private async void PlayOnXbox_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.PlayOnXboxAsync();

    private void GuidedMode_Click(object sender, RoutedEventArgs e)
    {
        GuidedEditor.Visibility = Visibility.Visible;
        ExpertEditor.Visibility = Visibility.Collapsed;
        GuidedModeButton.Background =
            (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
                "StudioAccentDimBrush"];
        GuidedModeButton.BorderBrush =
            (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
                "StudioAccentBrush"];
        ExpertModeButton.ClearValue(BackgroundProperty);
        ExpertModeButton.ClearValue(BorderBrushProperty);
    }

    private void ExpertMode_Click(object sender, RoutedEventArgs e)
    {
        GuidedEditor.Visibility = Visibility.Collapsed;
        ExpertEditor.Visibility = Visibility.Visible;
        ExpertModeButton.Background =
            (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
                "StudioAccentDimBrush"];
        ExpertModeButton.BorderBrush =
            (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources[
                "StudioAccentBrush"];
        GuidedModeButton.ClearValue(BackgroundProperty);
        GuidedModeButton.ClearValue(BorderBrushProperty);
    }

    private void Evidence_Click(object sender, RoutedEventArgs e) =>
        _navigate("evidence");
}
