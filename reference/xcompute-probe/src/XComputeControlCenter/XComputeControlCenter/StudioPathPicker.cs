using Microsoft.UI.Xaml;
using Windows.Storage.Pickers;

namespace XComputeControlCenter;

public interface IStudioPathPicker
{
    Task<string?> PickFolderAsync();

    Task<string?> PickJsonFileAsync();
}

public sealed class StudioPathPicker : IStudioPathPicker
{
    private readonly Window _window;

    public StudioPathPicker(Window window)
    {
        _window = window;
    }

    public async Task<string?> PickFolderAsync()
    {
        var picker = new FolderPicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            ViewMode = PickerViewMode.List,
        };
        picker.FileTypeFilter.Add("*");
        Initialize(picker);
        var folder = await picker.PickSingleFolderAsync();
        return folder?.Path;
    }

    public async Task<string?> PickJsonFileAsync()
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            ViewMode = PickerViewMode.List,
        };
        picker.FileTypeFilter.Add(".json");
        Initialize(picker);
        var file = await picker.PickSingleFileAsync();
        return file?.Path;
    }

    private void Initialize(object picker)
    {
        var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(_window);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, windowHandle);
    }
}
