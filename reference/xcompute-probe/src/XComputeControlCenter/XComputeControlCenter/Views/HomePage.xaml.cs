using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class HomePage : Page
{
    private readonly Action<string> _navigate;

    public MainWindowViewModel ViewModel { get; }

    public HomePage(MainWindowViewModel viewModel, Action<string> navigate)
    {
        ViewModel = viewModel;
        _navigate = navigate;
        InitializeComponent();
    }

    private void Create_Click(object sender, RoutedEventArgs e) =>
        _navigate("create");

    private void Adapt_Click(object sender, RoutedEventArgs e) =>
        _navigate("adapt");

    private async void Playtest_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(ViewModel.ProjectDirectory))
        {
            _navigate("projects");
            return;
        }
        if (string.IsNullOrWhiteSpace(ViewModel.DeviceAddress))
        {
            _navigate("devices");
            return;
        }
        await ViewModel.PlayOnXboxAsync();
    }

    private void Device_Click(object sender, RoutedEventArgs e) =>
        _navigate("devices");

    private void Evidence_Click(object sender, RoutedEventArgs e) =>
        _navigate("evidence");
}
