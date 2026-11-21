using Avalonia;

namespace PSVulkanPlay;

internal static class Program
{
    // Nothing that touches Avalonia may run before BuildAvaloniaApp: the platform is not initialized yet.
    [STAThread]
    public static int Main(string[] args)
    {
        // A shortcut, a launcher or a link that asks for a session: straight into it, no window of the app's.
        string? console = Core.QuickPlay.ConsoleAskedFor(args, out string error);
        if (error.Length > 0)
        {
            Console.Error.WriteLine($"PS VulkanPlay: {error}");
            return 64;
        }
        if (console is not null) return QuickPlay(console);

        Platform.NativeLibs.BindHarfBuzzToItself();
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    private static int QuickPlay(string console)
    {
        var runner = new Core.ProgramRunner();
        Core.AppSettings settings = new Core.SettingsStore(Core.SettingsStore.DefaultFile()).Load();
        using var stop = new CancellationTokenSource();
        // Ended from outside (the launcher's "stop", a signal): the session says goodbye to its console first.
        using var terminating = System.Runtime.InteropServices.PosixSignalRegistration.Create(System.Runtime.InteropServices.PosixSignal.SIGTERM, context =>
        {
            context.Cancel = true;
            stop.Cancel();
        });
        using var interrupted = System.Runtime.InteropServices.PosixSignalRegistration.Create(System.Runtime.InteropServices.PosixSignal.SIGINT, context =>
        {
            context.Cancel = true;
            stop.Cancel();
        });
        return Core.QuickPlay.RunAsync(runner, settings, console, Console.Error.WriteLine, stop.Token).GetAwaiter().GetResult();
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .With(new X11PlatformOptions { WmClass = Platform.DesktopIdentity.AppId })
            .WithInterFont()
            .LogToTrace();
}
