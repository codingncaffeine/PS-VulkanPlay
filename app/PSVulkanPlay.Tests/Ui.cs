using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PSVulkanPlay.ViewModels;
using PSVulkanPlay.Views;
using Xunit;

namespace PSVulkanPlay.Tests;

public static class TestEntry
{
    // The real app with its real drawing (Skia), on a screen that exists only in memory.
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UseSkia().UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false }).WithInterFont();
}

/// <summary>The window, driven the way a person drives it: pointer presses and typed text.</summary>
public static class Ui
{
    private static readonly HeadlessUnitTestSession Session = Start();

    // As the app's own start does, and for the same reason: the text shaper the app brings must be
    // opened before the system's font library can open the system's (Platform/NativeLibs.cs).
    // Without it this program dies at its first window where the system has its own text shaper
    // and a font library that opens it (Alpine, for one).
    private static HeadlessUnitTestSession Start()
    {
        Platform.NativeLibs.BindHarfBuzzToItself();
        return HeadlessUnitTestSession.StartNew(typeof(TestEntry));
    }

    /// <summary>
    /// Runs a test's body on the UI thread, to its end. The body is not async: a session's
    /// Dispatch awaits only the first step of an async body, and a test would pass whatever it
    /// asserts after that. Waiting is done by <see cref="Until"/>, which runs the UI's own work.
    /// </summary>
    public static void Run(Action body) => Session.Dispatch(() => { body(); return true; }, CancellationToken.None).GetAwaiter().GetResult();

    public static void Until(Func<bool> met, string what)
    {
        var limit = DateTime.UtcNow.AddSeconds(15);
        for (;;)
        {
            Dispatcher.UIThread.RunJobs();
            if (met()) return;
            Assert.True(DateTime.UtcNow < limit, "it did not happen: " + what);
            Thread.Sleep(5);
        }
    }

    public static void Settle()
    {
        for (int i = 0; i < 3; ++i)
        {
            Dispatcher.UIThread.RunJobs();
            AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
    }

    /// <summary>The app's window on a stand-in program, with the settings in a folder of its own.</summary>
    public static (MainWindow Window, MainViewModel Model) Open(FakeRunner fake, string settingsFolder, bool dark = true)
    {
        App.RunnerOverride = fake;
        App.SettingsFileOverride = Path.Combine(settingsFolder, "app.json");
        var app = (App)Application.Current!;
        MainViewModel model = app.CreateModel();
        app.ApplyTheme(dark ? "slate" : "light");
        var window = new MainWindow { DataContext = model };
        window.Show();
        Settle();
        return (window, model);
    }

    public static Button ButtonSaying(Window window, string text) =>
        window.GetVisualDescendants().OfType<Button>().First(button => button.Content as string == text && button.IsEffectivelyVisible);

    public static bool Shows(Window window, string text) =>
        window.GetVisualDescendants().OfType<TextBlock>().Any(block => block.IsEffectivelyVisible && (block.Text ?? "").Contains(text, StringComparison.Ordinal));

    /// <summary>A press and a release in the middle of the control, where the pointer really is.</summary>
    public static void Click(Window window, Control control)
    {
        Settle();
        Assert.True(control.IsEffectivelyVisible, $"{control.Name ?? control.GetType().Name} is not on the screen");
        Point? middle = control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), window);
        Assert.NotNull(middle);
        window.MouseMove(middle.Value);
        window.MouseDown(middle.Value, MouseButton.Left);
        window.MouseUp(middle.Value, MouseButton.Left);
        Settle();
    }

    public static void Type(Window window, TextBox box, string text)
    {
        Click(window, box);
        Assert.True(box.IsFocused, $"{box.Name} did not take the keyboard");
        window.KeyTextInput(text);
        Settle();
    }

    /// <summary>The window as drawn, kept as a picture for a person to look at.</summary>
    public static string Capture(Window window, string name)
    {
        Settle();
        string folder = Environment.GetEnvironmentVariable("PSVP_SHOTS") ?? Path.Combine(AppContext.BaseDirectory, "shots");
        Directory.CreateDirectory(folder);
        string file = Path.Combine(folder, name + ".png");
        using WriteableBitmap? frame = window.CaptureRenderedFrame();
        Assert.NotNull(frame);
#pragma warning disable CS0618   // the plain PNG it writes by default is what is wanted
        frame.Save(file);
#pragma warning restore CS0618
        return file;
    }
}
