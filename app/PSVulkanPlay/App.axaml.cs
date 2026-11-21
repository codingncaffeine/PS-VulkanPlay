using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PSVulkanPlay.Core;
using PSVulkanPlay.Theming;
using PSVulkanPlay.ViewModels;
using PSVulkanPlay.Views;

namespace PSVulkanPlay;

public sealed class App : Application
{
    /// <summary>For tests: the runner and the settings file the app starts with instead of the real ones.</summary>
    public static IProgramRunner? RunnerOverride { get; set; }
    public static string? SettingsFileOverride { get; set; }
    public static PsnSignIn? SignInOverride { get; set; }
    public static Shortcuts? ShortcutsOverride { get; set; }

    private string _theme = Themes.FollowDesktop;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
        // Colors before any window: a view made before them would have none.
        Themes.Apply(this, Themes.Slate);
        // A desktop that goes dark or light takes the app with it, when the app follows it.
        if (PlatformSettings is { } platform) platform.ColorValuesChanged += (_, _) => { if (_theme == Themes.FollowDesktop) ApplyTheme(_theme); };
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            MainViewModel model = CreateModel();
            desktop.MainWindow = new MainWindow { DataContext = model };
            // However the app ends (its window, the desktop's "quit", a signal): a session that runs
            // says goodbye to its console, and nothing this app started is left behind.
            AppDomain.CurrentDomain.ProcessExit += (_, _) => model.Shutdown();
        }
        base.OnFrameworkInitializationCompleted();
    }

    public MainViewModel CreateModel()
    {
        IProgramRunner runner = RunnerOverride ?? new ProgramRunner();
        // The desktop's own applications folder only for the real app: a test's settings file has its shortcuts beside it.
        Shortcuts? shortcuts = ShortcutsOverride ?? (SettingsFileOverride is null ? new Shortcuts(Shortcuts.DefaultFolder(), Shortcuts.ProgramForEntries(runner.Location)) : null);
        var model = new MainViewModel(runner, new SettingsStore(SettingsFileOverride ?? SettingsStore.DefaultFile()),
                                      action => Dispatcher.UIThread.Post(action), SignInOverride, shortcuts);
        ApplyTheme(model.Settings.Current.Theme);
        model.Settings.ThemeChanged += ApplyTheme;
        return model;
    }

    /// <summary>The look a setting stands for, on everything at once.</summary>
    public void ApplyTheme(string theme)
    {
        _theme = theme;
        Themes.Apply(this, Themes.Resolve(theme, Themes.DesktopIsDark(this)));
    }
}
