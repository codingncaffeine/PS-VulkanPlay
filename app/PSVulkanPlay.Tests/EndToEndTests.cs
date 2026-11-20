using Avalonia.Controls;
using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using PSVulkanPlay.Views;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>
/// The app's window on the REAL stream program, against the stand-in console: what the other
/// tests take on trust (that the program understands what the app asks, and the app what the
/// program answers). Runs only where that is set up (tests/scenario/phase5a.sh, behind the wall):
/// PSVP_E2E_NATIVE names the program, and a console called "Living Room" is registered.
/// </summary>
public class EndToEndTests
{
    private static string? Native => Environment.GetEnvironmentVariable("PSVP_E2E_NATIVE");

    [Fact]
    public void The_app_plays_on_the_stand_in_console_with_the_real_program() => Ui.Run(() =>
    {
        if (string.IsNullOrEmpty(Native)) Assert.Skip("needs the stream program and a stand-in console: run tests/scenario/phase5a.sh behind the wall");
        Environment.SetEnvironmentVariable("PSVP_NATIVE", Native);
        string folder = Path.Combine(Path.GetTempPath(), "psvp-e2e-" + Guid.NewGuid().ToString("N"));
        App.RunnerOverride = new ProgramRunner();
        App.SettingsFileOverride = Path.Combine(folder, "app.json");
        var app = (App)Avalonia.Application.Current!;
        MainViewModel model = app.CreateModel();
        var window = new MainWindow { DataContext = model };
        window.Show();
        Assert.Equal(Native, model.ProgramLocation);

        // The registered console is on the page, and the program found it switched on.
        Ui.Until(() => Ui.Shows(window, "Living Room") && model.Consoles.Count == 1 && model.Consoles[0].IsReady, "the registered console is on the page and on");
        // No controller of this machine can be seen from behind the wall, and the program says so.
        Ui.Until(() => Ui.Shows(window, "No controller connected"), "the bottom line says that no controller is there");

        Ui.Click(window, Ui.ButtonSaying(window, "Play"));
        Ui.Until(() => model.StreamShown, "the stream's own window showed its first picture");
        Assert.Equal(WindowState.Minimized, window.WindowState);
        // A second of the stream, then the session is ended from the app: the program says goodbye to the console.
        Thread.Sleep(1000);
        model.Stop.Execute(null);
        Ui.Until(() => !model.Busy, "the session ended");
        Assert.False(model.NoticeIsError, model.Notice + "\n" + model.Report);
        Assert.Equal("The session with Living Room has ended.", model.Notice);
        Assert.Contains("Played (H.264", model.Report);
        Assert.Contains("Goodbye sent and acknowledged.", model.Report);
        Ui.Until(() => Ui.Shows(window, "No controller connected"), "the controllers are watched again");
        window.Close();
        if (Directory.Exists(folder)) Directory.Delete(folder, true);
    });

    [Fact]
    public void A_console_that_refuses_gives_the_programs_reason_on_the_page() => Ui.Run(() =>
    {
        if (string.IsNullOrEmpty(Native) || Environment.GetEnvironmentVariable("PSVP_E2E_IN_USE") != "1")
            Assert.Skip("needs a stand-in console that is in use: run tests/scenario/phase5a.sh behind the wall");
        Environment.SetEnvironmentVariable("PSVP_NATIVE", Native);
        string folder = Path.Combine(Path.GetTempPath(), "psvp-e2e-" + Guid.NewGuid().ToString("N"));
        App.RunnerOverride = new ProgramRunner();
        App.SettingsFileOverride = Path.Combine(folder, "app.json");
        new SettingsStore(App.SettingsFileOverride).Save(new AppSettings { WakeFirst = false });   // no minute and a half of asking again
        var app = (App)Avalonia.Application.Current!;
        MainViewModel model = app.CreateModel();
        var window = new MainWindow { DataContext = model };
        window.Show();
        Ui.Until(() => model.Consoles.Count == 1, "the registered console is on the page");
        Ui.Click(window, Ui.ButtonSaying(window, "Play"));
        Ui.Until(() => !model.Busy && model.HasNotice, "the session's failure reached the page");
        Assert.True(model.NoticeIsError);
        Assert.Equal("Living Room: Remote Play is already in use on this console.", model.Notice);
        Assert.True(Ui.Shows(window, "Remote Play is already in use on this console."));
        window.Close();
        if (Directory.Exists(folder)) Directory.Delete(folder, true);
    });
}
