using Avalonia;

namespace PSVulkanPlay;

internal static class Program
{
    // Nothing that touches Avalonia may run before BuildAvaloniaApp: the platform is not initialized yet.
    [STAThread]
    public static void Main(string[] args)
    {
        Platform.NativeLibs.BindHarfBuzzToItself();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .With(new X11PlatformOptions { WmClass = Platform.DesktopIdentity.AppId })
            .WithInterFont()
            .LogToTrace();
}
