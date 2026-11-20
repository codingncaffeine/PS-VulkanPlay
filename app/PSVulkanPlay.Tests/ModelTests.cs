using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>The app's model with a stand-in program and no window: what it asks, and what it then says.</summary>
public class ModelTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), "psvp-model-" + Guid.NewGuid().ToString("N"));

    private readonly FakeService _service = new();

    private readonly List<MainViewModel> _models = [];

    private MainViewModel Model(FakeRunner fake)
    {
        var model = new MainViewModel(fake, new SettingsStore(Path.Combine(_folder, "app.json")), action => action(), new PsnSignIn(_service, "https://service.test/oauth"));
        _models.Add(model);
        return model;
    }

    /// <summary>Stands in for the window with the login page: answers with where the page led.</summary>
    private sealed class FakeSignInWindow(SignInAnswer answer) : ISignInWindow
    {
        public Uri? Shown { get; private set; }
        public Task<SignInAnswer> ShowAsync(Uri loginPage, string redirectAddress)
        {
            Shown = loginPage;
            return Task.Run(() => answer);
        }
    }

    public void Dispose()
    {
        // As the app's end does: what is still being saved is saved before the folder goes.
        foreach (MainViewModel model in _models) model.Shutdown();
        if (Directory.Exists(_folder)) Directory.Delete(_folder, true);
    }

    private static void Until(Func<bool> met, string what)
    {
        var limit = DateTime.UtcNow.AddSeconds(10);
        while (!met())
        {
            Assert.True(DateTime.UtcNow < limit, "it did not happen: " + what);
            Thread.Sleep(5);
        }
    }

    [Fact]
    public async Task The_home_page_lists_the_consoles_with_how_they_are()
    {
        MainViewModel model = Model(FakeRunner.WithTwoConsoles());
        Assert.False(model.Loaded);
        await model.RefreshAsync();
        Assert.True(model.Loaded);
        Assert.False(model.ShowEmpty);
        Assert.Equal(["Living Room", "Den"], model.Consoles.Select(card => card.Name));
        Assert.Equal(["On", "In rest mode"], model.Consoles.Select(card => card.StateText));
        Assert.True(model.Consoles[1].IsPs4);
        Assert.False(model.Consoles[0].CanWake);   // it is on
        Assert.True(model.Consoles[1].CanWake);
        // A second look keeps the cards that are there.
        ConsoleCard first = model.Consoles[0];
        await model.RefreshAsync();
        Assert.Same(first, model.Consoles[0]);
    }

    [Fact]
    public async Task With_no_console_registered_the_page_says_so_and_only_after_asking()
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, "[]", "");
        MainViewModel model = Model(fake);
        Assert.False(model.ShowEmpty);
        await model.RefreshAsync();
        Assert.True(model.ShowEmpty);
        Assert.DoesNotContain(fake.Calls, call => call[0] == "discover");
    }

    [Fact]
    public async Task Play_starts_the_program_with_the_settings_and_follows_what_it_says()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        model.Settings.Cushion = 20;
        ConsoleCard card = model.Consoles[0];
        card.Play.Execute(null);
        Assert.True(model.Busy && model.Playing);
        Assert.False(card.CanPlay);
        Assert.False(model.Consoles[1].Play.CanExecute(null));   // one session at a time
        Assert.Equal(["play", "Living Room", "--host", "192.0.2.10", "--wake", "--resolution", "1080", "--fps", "60", "--codec", "hevc", "--controller", "auto", "--cushion", "20", "--haptics-volume", "100"],
                     fake.Last("play"));
        Assert.Equal("Connecting to Living Room", model.SessionTitle);

        fake.Session!.Say("Living Room is in rest mode: waking it.");
        Assert.Equal("Living Room is in rest mode: waking it.", model.SessionLine);
        fake.Session.Say("2026-01-01T00:00:00.000Z INFO  window: SDL 3, video driver x11");   // a log line is not for the page
        Assert.Equal("Living Room is in rest mode: waking it.", model.SessionLine);
        Assert.False(model.StreamShown);
        fake.Session.Say("2026-01-01T00:00:01.000Z INFO  render: first picture shown, 1920x1080, 8 bits");
        Assert.True(model.StreamShown);
        Assert.Equal("Playing on Living Room", model.SessionTitle);

        fake.Session.End(0, "Played (HEVC): 100 pictures\nGoodbye sent and acknowledged.\n");
        Until(() => !model.Busy, "the session's end reached the model");
        Assert.False(model.StreamShown);
        Assert.Equal("The session with Living Room has ended.", model.Notice);
        Assert.False(model.NoticeIsError);
        Assert.Contains("Played (HEVC)", model.Report);
        Assert.True(card.CanPlay);
    }

    [Fact]
    public async Task A_session_that_does_not_start_shows_the_programs_own_reason()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        model.Consoles[0].Play.Execute(null);
        fake.Session!.End(1, "Living Room is not taking a session yet: asking again in 5 seconds.\n",
                          "2026-01-01T00:00:00.000Z INFO  ps-vulkanplay 0.0.1: play Living Room\n" +
                          "Living Room refused the connection: it is on, but is not taking Remote Play sessions at this moment (it may still be starting).\n" +
                          "Living Room: Remote Play is already in use on this console.\n" +
                          "2026-01-01T00:01:40.000Z INFO  play ended with code 1\n");
        Until(() => !model.Busy, "the session's end reached the model");
        Assert.True(model.NoticeIsError);
        Assert.Equal("Living Room: Remote Play is already in use on this console.", model.Notice);
        Assert.True(model.HasReport);
    }

    [Fact]
    public async Task A_reason_over_two_lines_is_shown_whole()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        model.Consoles[1].Play.Execute(null);
        fake.Session!.End(1, "", "Den did not come on within 60 seconds. It wakes only if rest mode keeps it connected to the\nnetwork (on the console: the rest mode settings), and only for a client it is registered with.\n");
        Until(() => !model.Busy, "the session's end reached the model");
        Assert.StartsWith("Den did not come on within 60 seconds.", model.Notice);
        Assert.EndsWith("only for a client it is registered with.", model.Notice);
    }

    [Fact]
    public async Task Ending_the_session_from_the_app_asks_the_program_to_stop()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        Assert.False(model.Stop.CanExecute(null));
        model.Consoles[0].Play.Execute(null);
        Assert.True(model.Stop.CanExecute(null));
        model.Stop.Execute(null);
        Assert.True(fake.Session!.Stopped);
        Until(() => !model.Busy, "the session's end reached the model");
        Assert.False(model.Stop.CanExecute(null));
    }

    [Fact]
    public async Task Wake_asks_the_console_at_its_address_and_says_what_came_of_it()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        fake.Answers["wake"] = _ => new RunResult(0, "Den is on.\n", "");
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        model.Consoles[1].Wake.Execute(null);
        Until(() => !model.Busy && model.Notice == "Den is on.", "the wake-up's answer reached the model");
        Assert.Equal(["wake", "Den", "--wait", "60", "--host", "192.0.2.11"], fake.Last("wake"));
        Assert.False(model.NoticeIsError);
    }

    [Fact]
    public async Task Forget_is_asked_twice_and_then_done()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        MainViewModel model = Model(fake);
        await model.RefreshAsync();
        ConsoleCard card = model.Consoles[1];
        card.AskForget.Execute(null);
        Assert.True(card.ConfirmForget);
        Assert.Null(fake.Last("forget"));
        card.CancelForget.Execute(null);
        Assert.False(card.ConfirmForget);
        card.AskForget.Execute(null);
        fake.Answers["consoles"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"}]""", "");
        card.Forget.Execute(null);
        Until(() => model.Consoles.Count == 1, "the forgotten console left the page");
        Assert.Equal(["forget", "Den"], fake.Last("forget"));
    }

    [Fact]
    public void Adding_a_console_goes_from_the_console_to_the_account_to_the_pin()
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, "[]", "");
        fake.Answers["account"] = _ => new RunResult(0, """{"account_id":"ywT7cR8BAAA="}""", "");
        fake.Answers["discover"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":false,"supported":true}]""", "");
        fake.Answers["register"] = arguments => arguments.Contains("12345678")
            ? new RunResult(0, "Registered with Living Room (PS5).\n", "")
            : new RunResult(1, "", "Living Room: the PIN is not the one the console shows.\nCheck the PIN (the console shows a new one each time).\n");
        MainViewModel model = Model(fake);
        model.GoAddConsole.Execute(null);
        AddConsoleViewModel add = model.AddConsole;
        Assert.True(model.IsAddConsole && add.IsStep1);
        Until(() => add.Found.Count == 1 && add.AccountId.Length > 0, "the console and the account were found");
        add.Choose.Execute(add.Found[0]);
        Assert.True(add.IsStep2);
        Assert.Equal("Living Room", add.ChosenName);
        Assert.True(add.AccountIdLooksRight);   // the one the program has
        add.AccountId = "not an id";
        Assert.False(add.Next.CanExecute(null));
        add.AccountId = "1234567890123";
        add.Next.Execute(null);
        Assert.True(add.IsStep3);
        add.Pin = "1234";
        Assert.False(add.Register.CanExecute(null));
        add.Pin = "8765 4321";
        add.Register.Execute(null);
        Until(() => add.HasError, "the console's refusal was shown");
        Assert.StartsWith("Living Room: the PIN is not the one the console shows.", add.Error);
        Assert.True(model.IsAddConsole);
        add.Pin = "1234 5678";
        fake.Answers["consoles"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"}]""", "");
        add.Register.Execute(null);
        Until(() => model.IsHome, "the page went home after registering");
        Assert.Equal(["register", "--host", "192.0.2.10", "--pin", "12345678", "--account-id", "1234567890123"], fake.Last("register"));
        Assert.Equal("Living Room is registered. Press Play to start.", model.Notice);
        Until(() => model.Consoles.Count == 1, "the new console is on the home page");
    }

    private static readonly FoundConsole LivingRoom = new("Living Room", "PS5", "ready", "192.0.2.10", "02005e102030", "", false, true);

    [Fact]
    public void Signing_in_takes_the_account_from_the_sign_in_and_goes_on_to_the_pin()
    {
        var fake = new FakeRunner();
        MainViewModel model = Model(fake);
        AddConsoleViewModel add = model.AddConsole;
        var window = new FakeSignInWindow(new SignInAnswer("https://remoteplay.dl.playstation.net/remoteplay/redirect?code=one-time&cid=1"));
        add.SignInWindow = window;
        add.Choose.Execute(LivingRoom);
        Assert.True(add.IsStep2);
        add.SignIn.Execute(null);
        Until(() => add.IsStep3, "the sign-in led on to the PIN");
        Assert.Contains("/authorize?", window.Shown!.AbsoluteUri);
        Assert.Equal("ywT7cR8BAAA=", add.AccountId);
        Assert.True(add.SignedIn);
        Assert.False(add.SigningIn);
        Assert.Contains("code=one-time", _service.Asked[0].Body);
        // The program keeps the account too: the next console does not ask again.
        Assert.Equal(["account", "ywT7cR8BAAA="], fake.Last("account"));
    }

    [Fact]
    public void A_sign_in_window_that_is_closed_changes_nothing_and_one_that_fails_says_why()
    {
        var fake = new FakeRunner();
        MainViewModel model = Model(fake);
        AddConsoleViewModel add = model.AddConsole;
        add.Choose.Execute(LivingRoom);
        add.SignInWindow = new FakeSignInWindow(new SignInAnswer(""));
        add.SignIn.Execute(null);
        Until(() => !add.SigningIn, "the closed window was noticed");
        Assert.True(add.IsStep2 && !add.HasError && add.AccountId.Length == 0);
        Assert.Empty(_service.Asked);

        _service.Answer = (_, _) => FakeService.Json("""{"error":"invalid_grant"}""", System.Net.HttpStatusCode.BadRequest);
        add.SignInWindow = new FakeSignInWindow(new SignInAnswer("https://remoteplay.dl.playstation.net/remoteplay/redirect?code=used"));
        add.SignIn.Execute(null);
        Until(() => add.HasError, "the service's refusal was shown");
        Assert.True(add.IsStep2);
        Assert.Contains("answered 400", add.Error);
        Assert.Null(fake.Last("account"));

        // A page that ended somewhere else is not taken for a sign-in.
        add.SignInWindow = new FakeSignInWindow(new SignInAnswer("https://example.com/remoteplay/redirect?code=stolen"));
        add.SignIn.Execute(null);
        Until(() => !add.SigningIn && add.Error.StartsWith("The sign-in did not end", StringComparison.Ordinal), "the wrong page was refused");
        Assert.Single(_service.Asked);   // nothing was sent for it
    }

    [Fact]
    public void Without_a_window_for_the_page_the_browser_is_opened_and_the_address_is_taken_from_the_clipboard()
    {
        var fake = new FakeRunner();
        MainViewModel model = Model(fake);
        AddConsoleViewModel add = model.AddConsole;
        Uri? opened = null;
        add.OpenBrowser = address => opened = address;
        add.SignInWindow = new FakeSignInWindow(new SignInAnswer("", "no web engine here"));
        add.Choose.Execute(LivingRoom);
        add.SignIn.Execute(null);
        Until(() => add.ShowPaste && opened is not null, "the sign-in went to the browser");
        Assert.Contains("/authorize?", opened!.AbsoluteUri);
        Assert.False(add.UsePasted.CanExecute(null));
        // What is on the clipboard is taken only when it is the address a sign-in ends on.
        add.LookAt("some text that was copied for something else");
        Assert.Equal("", add.Pasted);
        Assert.Empty(_service.Asked);
        add.LookAt(" https://remoteplay.dl.playstation.net/remoteplay/redirect?code=from-the-browser ");
        Until(() => add.IsStep3, "the copied address led on to the PIN");
        Assert.Contains("code=from-the-browser", _service.Asked[0].Body);
        Assert.Equal("ywT7cR8BAAA=", add.AccountId);
        Assert.False(add.ShowPaste);
    }

    [Fact]
    public void A_console_that_is_already_added_or_too_old_is_not_taken()
    {
        var fake = new FakeRunner();
        MainViewModel model = Model(fake);
        AddConsoleViewModel add = model.AddConsole;
        add.Choose.Execute(new FoundConsole("Living Room", "PS5", "ready", "192.0.2.10", "02005e102030", "", true, true));
        Assert.True(add.IsStep1);
        Assert.Equal("Living Room is already registered here.", add.Error);
        add.Choose.Execute(new FoundConsole("Old", "PS4", "ready", "192.0.2.12", "02005e102032", "", false, false));
        Assert.True(add.IsStep1);
        Assert.Contains("too old", add.Error);
    }

    [Fact]
    public void An_address_nobody_answers_at_says_so()
    {
        var fake = new FakeRunner();
        fake.Answers["discover"] = _ => new RunResult(1, "[]", "No console answered at 192.0.2.99.\n");
        MainViewModel model = Model(fake);
        AddConsoleViewModel add = model.AddConsole;
        add.Address = " 192.0.2.99 ";
        add.UseAddress.Execute(null);
        Until(() => add.HasError, "the missing answer was shown");
        Assert.StartsWith("No console answered at 192.0.2.99.", add.Error);
        Assert.Contains(fake.Calls, call => call[0] == "discover" && call.Contains("192.0.2.99"));
    }

    [Fact]
    public void Settings_are_kept_the_moment_they_change()
    {
        var fake = new FakeRunner();
        MainViewModel model = Model(fake);
        string file = model.Settings.File;
        model.Settings.Cushion = 20;
        model.Settings.BackLeft = model.Settings.Buttons.First(choice => choice.Value == "cross");
        model.Settings.AutoBitrate = false;
        model.Settings.BitrateMbps = 22;
        Until(() => File.Exists(file) && new SettingsStore(file).Load() is { CushionMs: 20, BackLeft: "cross", BitrateKbps: 22000 }, "the settings reached the file");
        Assert.Equal("20 ms of added delay", model.Settings.CushionText);
        model.Settings.Pacing = false;
        Assert.Equal("Off with even pacing", model.Settings.CushionText);
        Assert.Equal("22 Mbit/s", model.Settings.BitrateText);
    }
}
