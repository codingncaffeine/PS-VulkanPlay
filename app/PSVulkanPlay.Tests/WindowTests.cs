using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.VisualTree;
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
    public void A_console_goes_into_Steam_from_its_card() => Ui.Run(() =>
    {
        // A stand-in Steam folder with one account: never the user's.
        string root = Path.Combine(_folder, "Steam");
        Directory.CreateDirectory(Path.Combine(root, "userdata", "2222", "config"));
        App.SteamOverride = new SteamShortcuts(root, "/opt/PS VulkanPlay/ps-vulkanplay", () => false, SteamShortcuts.Resource("banner.png"), SteamShortcuts.Resource("icon.png"));
        try
        {
            FakeRunner fake = FakeRunner.WithTwoConsoles();
            (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
            Ui.Until(() => Ui.Shows(window, "In rest mode") && Ui.Shows(window, "On"), "the consoles are on the page");
            Ui.Click(window, Ui.ButtonSaying(window, "Add to Steam"));
            Ui.Until(() => Ui.Shows(window, "is in Steam's library now"), "the page says it is in Steam");
            Assert.True(model.Steam!.Has(model.Consoles.First(card => card.InSteam).Console));
            Assert.True(File.Exists(Path.Combine(root, "userdata", "2222", "config", "shortcuts.vdf")));
            Ui.Capture(window, "home-steam-dark");
            Ui.Click(window, Ui.ButtonSaying(window, "Remove from Steam"));
            Ui.Until(() => Ui.Shows(window, "is no longer in Steam's library"), "the page says it left Steam");
            window.Close();
        }
        finally
        {
            App.SteamOverride = null;
        }
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
    public void Diagnostics_say_where_pictures_are_decoded_and_how_the_last_session_ended() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        string? copied = null, opened = null;
        model.Diagnostics.CopyText = text => copied = text;
        model.Diagnostics.OpenFolder = folder => opened = folder;
        Ui.Click(window, window.FindControl<Button>("NavDiagnostics")!);
        Assert.True(model.IsDiagnostics);
        Ui.Until(() => Ui.Shows(window, "On NVIDIA GeForce RTX 5080 (H.264, HEVC). Each session checks the card's first two seconds against software decoding."),
                 "the page says the card decodes");
        Assert.True(Ui.Shows(window, "PS VulkanPlay 0.0.3  ·  FFmpeg 9.1  ·  SDL 3.4.20  ·  Vulkan loader 1.4.363"));
        Assert.True(Ui.Shows(window, "NVIDIA GeForce RTX 5080 (discrete GPU, Vulkan 1.4.357)"));
        Assert.StartsWith("The last session, started ", model.Diagnostics.LastSessionTitle);
        Assert.Contains("Smoothness: the picture stood for over 30 ms 330 times.", window.FindControl<TextBox>("LastSessionBox")!.Text);
        Assert.Equal(["diagnose", "--json"], fake.Last("diagnose"));
        Ui.Capture(window, "diagnostics-dark");
        Ui.Click(window, window.FindControl<Button>("DiagnosticsCopy")!);
        Assert.NotNull(copied);
        Assert.Contains("Decoding: On NVIDIA GeForce RTX 5080", copied);
        Assert.Contains("Played (HEVC", copied);
        Ui.Click(window, window.FindControl<Button>("OpenLogsButton")!);
        Assert.Equal("/home/someone/.local/state/ps-vulkanplay/logs", opened);

        // A computer whose card does not decode, and no session yet: said as such.
        fake.Answers["diagnose"] = _ => new RunResult(0, """{"version": "0.0.3","vulkan_loader": "1.4.363","vulkan_error": "","devices": [{"name": "llvmpipe","kind": "software","vulkan": "1.4.363"}],"gpu_decoding": "","gpu_decoding_why_not": "the device has no VK_KHR_video_queue","ffmpeg": "9.1","sdl": "3.4.20","last_session_started": "","last_session": [],"logs_folder": ""}""", "");
        Ui.Click(window, window.FindControl<Button>("DiagnosticsAgain")!);
        Ui.Until(() => Ui.Shows(window, "In software: the device has no VK_KHR_video_queue."), "the page says why it decodes in software");
        Assert.True(Ui.Shows(window, "No session was played yet"));
        Assert.False(window.FindControl<TextBox>("LastSessionBox")!.IsEffectivelyVisible);
        Assert.False(window.FindControl<Button>("OpenLogsButton")!.IsEffectivelyVisible);

        // The program that does not answer: said, the page keeps what it knew.
        fake.Answers["diagnose"] = _ => new RunResult(1, "", "diagnose: this build has no graphics part to ask\n");
        Ui.Click(window, window.FindControl<Button>("DiagnosticsAgain")!);
        Ui.Until(() => Ui.Shows(window, "The stream program did not answer: diagnose: this build has no graphics part to ask"), "the failure is said");
    });

    [Fact]
    public void One_console_gets_a_picture_of_its_own_on_the_settings_page() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => model.Consoles.Count == 2, "the two consoles are listed");
        Ui.Click(window, window.FindControl<Button>("NavSettings")!);
        Ui.Until(() => Ui.Shows(window, "One console's picture"), "the card is on the settings page");
        var console = window.FindControl<ComboBox>("ConsolePictureConsole")!;
        Assert.Equal(["Living Room", "Den"], model.Settings.ConsoleNames);
        console.SelectedItem = "Den";
        var own = window.FindControl<ToggleSwitch>("ConsoleOwnPictureSwitch")!;
        own.BringIntoView();
        Ui.Settle();
        Ui.Click(window, own);
        Assert.True(model.Settings.ConsoleOwnPicture);
        model.Settings.ConsoleResolution = model.Settings.Resolutions.First(choice => choice.Value == "720");
        model.Settings.ConsoleBitrate = model.Settings.Bitrates.First(choice => choice.Value == "10000");
        Ui.Settle();
        Ui.Capture(window, "settings-console-dark");
        Assert.Equal(720, model.Settings.Current.PerConsole["Den"].Resolution);
        // Living Room keeps the app's picture; switching between them shows each one's own.
        console.SelectedItem = "Living Room";
        Ui.Settle();
        Assert.False(model.Settings.ConsoleOwnPicture);
        Assert.Equal("1080", model.Settings.ConsoleResolution.Value);
        console.SelectedItem = "Den";
        Ui.Settle();
        Assert.True(model.Settings.ConsoleOwnPicture);
        Assert.Equal("720", model.Settings.ConsoleResolution.Value);
        // Off again: the app's picture for it too.
        Ui.Click(window, own);
        Assert.False(model.Settings.Current.PerConsole.ContainsKey("Den"));
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
        // The controller's card, its rows from the Fn buttons to the dead zone in view.
        model.Settings.DeadZone = 12;
        Ui.Until(() => Ui.Shows(window, "12% of a stick's throw counts as rest"), "the dead zone says what it does");
        window.GetVisualDescendants().OfType<TextBlock>().First(block => block.Text == "Stick dead zone").BringIntoView();
        Ui.Settle();
        Ui.Capture(window, "settings-controller-dark");
        model.Settings.DeadZone = 0;

        // The looks are at the end of the page: each is a small picture of itself, and one press puts it on.
        var cards = window.FindControl<ItemsControl>("ThemeCards")!;
        cards.BringIntoView();
        Ui.Settle();
        Button Swatch(string name) => cards.GetVisualDescendantButtons().First(button => (button.DataContext as ThemeChoice)?.Name == name);
        Assert.Equal(7, cards.GetVisualDescendantButtons().Count());
        Ui.Click(window, Swatch("Midnight Blue"));
        Assert.Equal("midnight", model.Settings.Theme);
        Assert.Equal(ThemeVariant.Dark, Application.Current!.RequestedThemeVariant);
        Assert.Equal(Color.Parse("#2B6CE6"), ((ISolidColorBrush)Application.Current.Resources["Accent"]!).Color);
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

    [Fact]
    public void High_contrast_and_a_larger_size_change_the_whole_window_at_once() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "Living Room"), "the home page is up");
        Button play = window.GetVisualDescendantButtons().First(button => button.Content as string == "Play");
        double before = play.Bounds.Width;
        var rootBefore = play.TranslatePoint(new Point(play.Bounds.Width, 0), window)!.Value.X - play.TranslatePoint(new Point(0, 0), window)!.Value.X;

        Ui.Click(window, window.FindControl<Button>("NavSettings")!);
        var cards = window.FindControl<ItemsControl>("ThemeCards")!;
        cards.BringIntoView();
        Ui.Settle();
        Ui.Click(window, cards.GetVisualDescendantButtons().First(button => (button.DataContext as ThemeChoice)?.Name == "High Contrast"));
        Assert.Equal("contrast", model.Settings.Theme);
        Assert.Equal(Color.Parse("#000000"), ((ISolidColorBrush)Application.Current!.Resources["WindowBg"]!).Color);
        Assert.Equal(Color.Parse("#FFFFFF"), ((ISolidColorBrush)Application.Current.Resources["Line"]!).Color);
        // A switch that is on shows its knob in black on the yellow, not the theme's white.
        Assert.Equal(Color.Parse("#000000"), ((ISolidColorBrush)Application.Current.Resources["ToggleSwitchKnobFillOn"]!).Color);

        var scale = window.FindControl<ComboBox>("ScaleChoice")!;
        Assert.Equal("100%", model.Settings.Scale.Label);
        model.Settings.Scale = model.Settings.Scales.First(choice => choice.Value == "150");
        Ui.Settle();
        Assert.Equal("150%", (scale.SelectedItem as Choice)?.Label);
        Ui.Capture(window, "settings-contrast-150");

        Ui.Click(window, window.FindControl<Button>("NavHome")!);
        Ui.Until(() => Ui.Shows(window, "Living Room"), "the home page is back");
        Ui.Settle();
        // The same control, one and a half times as wide on the screen; its own layout unchanged.
        double rootAfter = play.TranslatePoint(new Point(play.Bounds.Width, 0), window)!.Value.X - play.TranslatePoint(new Point(0, 0), window)!.Value.X;
        Assert.Equal(before, play.Bounds.Width, 0.5);
        Assert.InRange(rootAfter / rootBefore, 1.49, 1.51);
        Ui.Capture(window, "home-contrast-150");
        Ui.Until(() => File.Exists(model.Settings.File) && new SettingsStore(model.Settings.File).Load() is { ScalePercent: 150, Theme: "contrast" }, "the choices are kept");
        window.Close();
    });
}

internal static class VisualSearch
{
    public static IEnumerable<Button> GetVisualDescendantButtons(this Control control) =>
        Avalonia.VisualTree.VisualExtensions.GetVisualDescendants(control).OfType<Button>();
}
