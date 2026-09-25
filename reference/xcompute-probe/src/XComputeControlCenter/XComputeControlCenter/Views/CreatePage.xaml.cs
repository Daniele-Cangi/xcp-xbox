using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class CreatePage : Page
{
    private readonly Action<string> _navigate;
    private readonly IStudioPathPicker _picker;

    public MainWindowViewModel ViewModel { get; }

    public CreatePage(
        MainWindowViewModel viewModel,
        Action<string> navigate,
        IStudioPathPicker picker)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        _picker = picker;
        InitializeComponent();
    }

    private async void PickProjectParent_Click(
        object sender,
        RoutedEventArgs e)
    {
        var parent = await _picker.PickFolderAsync();
        if (parent is not null)
        {
            ViewModel.ProjectDirectory = Path.Combine(
                parent,
                ViewModel.ProjectId);
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

    private async void CreateProject_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.CreateProjectAsync();

    private async void InitializeLifecycle_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.InitializeLifecycleAsync();

    private void Workspace_Click(object sender, RoutedEventArgs e) =>
        _navigate("projects");
}
