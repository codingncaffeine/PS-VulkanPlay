using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Styling;
using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using PSVulkanPlay.Views;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>The real window on a stand-in program, walked with the pointer and the keyboard.</summary>
public class WindowTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), "psvp-window-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_folder)) Directory.Delete(_folder, true);
    }

    [Fact]
    public void Play_is_one_press_and_the_window_follows_the_session() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode") && Ui.Shows(window, "On"), "the two consoles are on the page with their states");
        Assert.True(Ui.Shows(window, "Living Room") && Ui.Shows(window, "Den"));

        // The bottom line: the controller a session would use, as the program reports it.
        Assert.True(Ui.Shows(window, "Looking for a controller"));
        Assert.Equal(["controllers", "--json", "--watch", "--presses"], fake.Last("controllers"));
        fake.Watch!.Say("""{"controllers": []}""");
        Ui.Until(() => Ui.Shows(window, "No controller connected"), "the page says that no controller is there");
        fake.Watch.Say("""{"controllers": [{"name": "DualSense Edge Wireless Controller", "kind": "dualsense-edge", "link": "cable", "battery": 55, "power": "charging", "touchpad": true, "motion": true, "rumble": true, "haptics": "sound-device"}]}""");
        Ui.Until(() => Ui.Shows(window, "DualSense Edge Wireless Controller  ·  on a cable  ·  55%, charging"), "the controller is on the bottom line");
        Assert.True(Ui.Shows(window, "Haptics and trigger effects"));
        Ui.Capture(window, "home-dark");

        FakeSession watch = fake.Watch;
        Ui.Click(window, Ui.ButtonSaying(window, "Play"));
        Ui.Until(() => fake.Last("play") is not null, "the program was started for a session");
        Assert.True(watch.Stopped);   // the stream has the controller now
        Assert.Equal("Living Room", fake.Last("play")![1]);
        Ui.Until(() => Ui.Shows(window, "Connecting to Living Room"), "the page says what it is doing");
        fake.Session!.Say("Living Room accepted the session (PS5).");
        Ui.Until(() => Ui.Shows(window, "Living Room accepted the session (PS5)."), "the program's own words are on the page");
        Ui.Capture(window, "session-dark");

        // The stream's window is up: this one gets out of its way, and comes back when the session ends.
        fake.Session.Say("2026-01-01T00:00:01.000Z INFO  render: first picture shown, 1920x1080");
        Ui.Until(() => window.WindowState == WindowState.Minimized, "the window got out of the stream's way");
        fake.Session.End(0, "Played (HEVC, FFmpeg's hevc decoder in software): 100 pictures decoded\nGoodbye sent and acknowledged.\n");
        Ui.Until(() => window.WindowState == WindowState.Normal && Ui.Shows(window, "The session with Living Room has ended."), "the window came back and says the session ended");
        Ui.Until(() => !ReferenceEquals(fake.Watch, watch), "the controllers are watched again after the session");
        Ui.Click(window, Ui.ButtonSaying(window, "What the program said"));
        Assert.True(model.ShowReport);
        Ui.Capture(window, "ended-dark");
        Ui.Click(window, Ui.ButtonSaying(window, "Dismiss"));
        Assert.False(model.HasNotice);
        window.Close();
    });

    [Fact]
    public void A_session_that_fails_says_why_on_the_page() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel _) = Ui.Open(fake, _folder, dark: false);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        Ui.Capture(window, "home-light");
        fake.Watch!.Say("""{"controllers": [{"name": "Wireless Controller", "kind": "dualshock4", "link": "wireless", "battery": 10, "power": "battery", "haptics": "none"}]}""");
        Ui.Until(() => Ui.Shows(window, "Its battery is low"), "a low battery is said on the bottom line");
        Ui.Click(window, Ui.ButtonSaying(window, "Play"));
        Ui.Until(() => fake.Session is not null, "the program was started for a session");
        fake.Session!.End(1, "", "Living Room: Remote Play is already in use on this console.\n");
        Ui.Until(() => Ui.Shows(window, "Remote Play is already in use on this console."), "the program's reason is on the page");
        Ui.Capture(window, "failed-light");
        window.Close();
    });

    [Fact]
    public void A_first_run_leads_through_adding_a_console() => Ui.Run(() =>
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, "[]", "");
        fake.Answers["discover"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":false,"supported":true}]""", "");
        fake.Answers["register"] = _ => new RunResult(0, "Registered with Living Room (PS5).\n", "");
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "No console yet"), "an empty home page says what to do");
        Ui.Capture(window, "empty-dark");

        Ui.Click(window, window.FindControl<Button>("AddFirst")!);
        Assert.True(model.IsAddConsole);
        Ui.Until(() => Ui.Shows(window, "192.0.2.10"), "the console that answered is listed");
        Ui.Capture(window, "add-1-dark");
        Ui.Click(window, window.FindControl<ItemsControl>("FoundList")!.GetVisualDescendantButtons().First());
        Assert.True(model.AddConsole.IsStep2);

        Assert.False(window.FindControl<Button>("NextButton")!.IsEffectivelyEnabled);   // no account ID yet
        Ui.Type(window, window.FindControl<TextBox>("AccountBox")!, "1234567890123");
        Ui.Capture(window, "add-2-dark");
        Ui.Click(window, window.FindControl<Button>("NextButton")!);
        Assert.True(model.AddConsole.IsStep3);

        Assert.False(window.FindControl<Button>("RegisterButton")!.IsEffectivelyEnabled);
        Ui.Type(window, window.FindControl<TextBox>("PinBox")!, "12345678");
        Ui.Capture(window, "add-3-dark");
        fake.Answers["consoles"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"}]""", "");
        Ui.Click(window, window.FindControl<Button>("RegisterButton")!);
        Ui.Until(() => model.IsHome && Ui.Shows(window, "Living Room is registered. Press Play to start."), "the new console is on the home page");
        Assert.Equal(["register", "--host", "192.0.2.10", "--pin", "12345678", "--account-id", "1234567890123"], fake.Last("register"));
        Ui.Until(() => Ui.Shows(window, "Play"), "the console's card is there");
        window.Close();
    });

    private sealed class SignInWindowThatSignsIn : ISignInWindow
    {
        public Task<SignInAnswer> ShowAsync(Uri loginPage, string redirectAddress) =>
            Task.Run(() => new SignInAnswer(redirectAddress + "?code=from-the-window&cid=1"));
    }

    private sealed class NoSignInWindow : ISignInWindow
    {
        public Task<SignInAnswer> ShowAsync(Uri loginPage, string redirectAddress) => Task.Run(() => new SignInAnswer("", "no web engine here"));
    }

    [Fact]
    public void Signing_in_with_psn_fills_in_the_account_and_leads_to_the_pin() => Ui.Run(() =>
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, "[]", "");
        fake.Answers["discover"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":false,"supported":true}]""", "");
        fake.Answers["register"] = _ => new RunResult(0, "Registered with Living Room (PS5).\n", "");
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "No console yet"), "an empty home page says what to do");
        Ui.Click(window, window.FindControl<Button>("AddFirst")!);
        Ui.Until(() => Ui.Shows(window, "192.0.2.10"), "the console that answered is listed");
        Ui.Click(window, window.FindControl<ItemsControl>("FoundList")!.GetVisualDescendantButtons().First());
        Assert.True(model.AddConsole.IsStep2);
        Ui.Capture(window, "add-2-sign-in-dark");

        // The road without a window for the page: the browser is opened, and the page says what to do.
        Uri? opened = null;
        model.AddConsole.OpenBrowser = address => opened = address;
        model.AddConsole.SignInWindow = new NoSignInWindow();
        Ui.Click(window, window.FindControl<Button>("SignInButton")!);
        Ui.Until(() => opened is not null && Ui.Shows(window, "In your browser"), "the sign-in went to the browser and the page says so");
        Ui.Capture(window, "add-2-browser-dark");
        Assert.False(window.FindControl<Button>("UsePastedButton")!.IsEffectivelyEnabled);

        // The main road: the login page in its own window.
        model.AddConsole.SignInWindow = new SignInWindowThatSignsIn();
        Ui.Click(window, window.FindControl<Button>("SignInButton")!);
        Ui.Until(() => model.AddConsole.IsStep3 && Ui.Shows(window, "Signed in: the account was taken from your sign-in."), "the sign-in led on to the PIN");
        Assert.Equal("ywT7cR8BAAA=", model.AddConsole.AccountId);
        Assert.Contains(Ui.Service.Asked, asked => asked.Body.Contains("code=from-the-window"));
        Ui.Capture(window, "add-3-signed-in-dark");

        Ui.Type(window, window.FindControl<TextBox>("PinBox")!, "12345678");
        fake.Answers["consoles"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"}]""", "");
        Ui.Click(window, window.FindControl<Button>("RegisterButton")!);
        Ui.Until(() => model.IsHome, "the page went home after registering");
        Assert.Equal(["register", "--host", "192.0.2.10", "--pin", "12345678", "--account-id", "ywT7cR8BAAA="], fake.Last("register"));
        window.Close();
    });

    [Fact]
    public void Settings_are_one_page_and_the_look_changes_at_once() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Click(window, window.FindControl<Button>("NavSettings")!);
        Assert.True(model.IsSettings);
        Ui.Until(() => Ui.Shows(window, "Smoothness") && Ui.Shows(window, "None: the least delay"), "the settings page is up");
        model.Settings.Cushion = 20;
        Ui.Until(() => Ui.Shows(window, "20 ms of added delay"), "the cushion says what it costs");
        Ui.Capture(window, "settings-dark");

        // The looks are at the end of the page: each is a small picture of itself, and one press puts it on.
        var cards = window.FindControl<ItemsControl>("ThemeCards")!;
        cards.BringIntoView();
        Ui.Settle();
        Button Swatch(string name) => cards.GetVisualDescendantButtons().First(button => (button.DataContext as ThemeChoice)?.Name == name);
        Assert.Equal(6, cards.GetVisualDescendantButtons().Count());
        Ui.Click(window, Swatch("Midnight Blue"));
        Assert.Equal("midnight", model.Settings.Theme);
        Assert.Equal(ThemeVariant.Dark, Application.Current!.RequestedThemeVariant);
        Assert.Equal(Color.Parse("#3B82F6"), ((ISolidColorBrush)Application.Current.Resources["Accent"]!).Color);
        Assert.True(Swatch("Midnight Blue").Classes.Contains("active") && !Swatch("Slate").Classes.Contains("active"));
        Ui.Capture(window, "look-midnight");
        Ui.Click(window, Swatch("Light"));
        Assert.Equal(ThemeVariant.Light, Application.Current.RequestedThemeVariant);
        Assert.Equal(Color.Parse("#F2F3F5"), ((ISolidColorBrush)Application.Current.Resources["WindowBg"]!).Color);
        Ui.Capture(window, "look-light");
        window.FindControl<ScrollViewer>("SettingsScroll")!.ScrollToHome();
        Ui.Capture(window, "settings-light");
        Ui.Until(() => File.Exists(model.Settings.File) && new SettingsStore(model.Settings.File).Load() is { CushionMs: 20, Theme: "light" }, "the choices are kept");
        window.Close();
    });
}

internal static class VisualSearch
{
    public static IEnumerable<Button> GetVisualDescendantButtons(this Control control) =>
        Avalonia.VisualTree.VisualExtensions.GetVisualDescendants(control).OfType<Button>();
}
