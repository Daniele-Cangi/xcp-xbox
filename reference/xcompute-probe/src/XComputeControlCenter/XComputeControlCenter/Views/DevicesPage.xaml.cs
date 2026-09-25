using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class DevicesPage : Page
{
    public MainWindowViewModel ViewModel { get; }

    public DevicesPage(MainWindowViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) =>
        await ViewModel.RefreshAsync();

    private async void EnrollTrust_Click(object sender, RoutedEventArgs e)
    {
        var pairingCode = PairingCodeBox.Password;
        PairingCodeBox.Password = string.Empty;
        try
        {
            await ViewModel.EnrollTrustedControllerAsync(pairingCode);
        }
        finally
        {
            pairingCode = string.Empty;
        }
    }

    private async void DiscoverHostProfile_Click(
        object sender,
        RoutedEventArgs e) =>
        await ViewModel.DiscoverHostProfileAsync();
}
