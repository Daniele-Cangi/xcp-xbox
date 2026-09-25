using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using XComputeControlCenter.Core;

namespace XComputeControlCenter;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        RequestedTheme = ApplicationTheme.Dark;
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            _window = new MainWindow
            {
                Title = "XCP Studio",
            };
        }
        catch (StudioToolchainException exception)
        {
            _window = CreateToolchainErrorWindow(exception);
        }
        catch (StudioPythonRuntimeException exception)
        {
            _window = CreatePythonRuntimeErrorWindow(exception);
        }
        _window.Activate();
    }

    private static Window CreatePythonRuntimeErrorWindow(
        StudioPythonRuntimeException exception)
    {
        var content = new StackPanel
        {
            Padding = new Thickness(32),
            Spacing = 12,
            Children =
            {
                new TextBlock
                {
                    Text = "Private Python runtime required",
                    FontSize = 28,
                    FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                },
                new TextBlock
                {
                    Text = exception.Code,
                    FontFamily = new Microsoft.UI.Xaml.Media.FontFamily(
                        "Cascadia Mono, Consolas"),
                },
                new TextBlock
                {
                    Text = exception.Message,
                    TextWrapping = TextWrapping.Wrap,
                },
                new TextBlock
                {
                    Text = exception.Details.Correction,
                    TextWrapping = TextWrapping.Wrap,
                },
                new TextBlock
                {
                    Text =
                        $"{StudioPythonRuntimeResolver.EnvironmentVariable} may point to an exact manifest-bound private runtime.",
                    TextWrapping = TextWrapping.Wrap,
                },
            },
        };
        return new Window
        {
            Title = "XCP Studio · Python runtime unavailable",
            Content = content,
        };
    }

    private static Window CreateToolchainErrorWindow(
        StudioToolchainException exception)
    {
        var correction = new TextBlock
        {
            Text = exception.Details.Correction,
            TextWrapping = TextWrapping.Wrap,
        };
        var content = new StackPanel
        {
            Padding = new Thickness(32),
            Spacing = 12,
            Children =
            {
                new TextBlock
                {
                    Text = "Portable toolchain required",
                    FontSize = 28,
                    FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                },
                new TextBlock
                {
                    Text = exception.Code,
                    FontFamily = new Microsoft.UI.Xaml.Media.FontFamily(
                        "Cascadia Mono, Consolas"),
                },
                new TextBlock
                {
                    Text = exception.Message,
                    TextWrapping = TextWrapping.Wrap,
                },
                correction,
                new TextBlock
                {
                    Text =
                        $"{StudioToolchainResolver.EnvironmentVariable} may point to an exact exported xcp-agent-kit-manifest.json root.",
                    TextWrapping = TextWrapping.Wrap,
                },
            },
        };
        return new Window
        {
            Title = "XCP Studio · toolchain unavailable",
            Content = content,
        };
    }
}
