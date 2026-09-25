using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.ViewModels;

namespace XComputeControlCenter.Views;

public sealed partial class EvidencePage : Page
{
    public MainWindowViewModel ViewModel { get; }

    public EvidencePage(MainWindowViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
    }
}
