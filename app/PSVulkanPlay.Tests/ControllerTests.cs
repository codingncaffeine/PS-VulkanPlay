using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.VisualTree;
using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using PSVulkanPlay.Views;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>
/// The app moved through with a controller: the lines the stream program says for its presses
/// go in where the program's would, and what is asserted is where the focus is and what the
/// window then did.
/// </summary>
public class ControllerTests : IDisposable
{
    private const string DualSense = """{"controllers": [{"name": "DualSense Edge Wireless Controller", "kind": "dualsense-edge", "link": "wireless", "battery": 85, "power": "battery", "touchpad": true, "motion": true, "rumble": true, "haptics": "rumble"}]}""";
    private const string DualSenseAndXbox = """{"controllers": [{"name": "DualSense Edge Wireless Controller", "kind": "dualsense-edge", "link": "wireless", "battery": 85, "power": "battery", "haptics": "rumble"}, {"name": "Xbox Wireless Controller", "kind": "xbox", "link": "cable", "battery": -1, "power": "", "haptics": "none"}]}""";

    private readonly string _folder = Path.Combine(Path.GetTempPath(), "psvp-controller-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(_folder)) Directory.Delete(_folder, true);
    }

    // A press, as the stream program says it; what it led to has happened when this returns.
    private static void Press(FakeRunner fake, string press, int controller = 0)
    {
        fake.Watch!.Say($$"""{"press":"{{press}}","controller":{{controller}}}""");
        Ui.Settle();
    }

    private static Control? Focused(MainWindow window) => window.Navigator.Focused;
    private static string FocusedText(MainWindow window) => Focused(window) is Button { Content: string text } ? text : Focused(window)?.Name ?? Focused(window)?.GetType().Name ?? "(nothing)";
    private static ConsoleCard? FocusedCard(MainWindow window) => Focused(window)?.DataContext as ConsoleCard;

    [Fact]
    public void The_programs_press_lines_are_read()
    {
        // The very lines the stream program's own test holds it to (tests/unit/test_controller_presses.cpp).
        Assert.Equal((ControllerPress.Down, 0), ControllerJson.ReadPress("""{"press":"down","controller":0}"""));
        Assert.Equal((ControllerPress.PreviousPage, 2), ControllerJson.ReadPress("""{"press":"previous","controller":2}"""));
        foreach ((string name, ControllerPress press) in new[]
                 {
                     ("up", ControllerPress.Up), ("down", ControllerPress.Down), ("left", ControllerPress.Left), ("right", ControllerPress.Right),
                     ("accept", ControllerPress.Accept), ("back", ControllerPress.Back), ("previous", ControllerPress.PreviousPage), ("next", ControllerPress.NextPage),
                 })
        {
            Assert.Equal((press, 1), ControllerJson.ReadPress($$"""{"press":"{{name}}","controller":1}"""));
        }
        Assert.Null(ControllerJson.ReadPress("""{"press":"sideways","controller":0}"""));
        Assert.Null(ControllerJson.ReadPress(DualSense));
        Assert.Null(ControllerJson.ReadPress("not JSON"));
        Assert.Null(ControllerJson.Read("""{"press":"down","controller":0}"""));
    }

    [Fact]
    public void Buttons_are_called_what_their_maker_calls_them()
    {
        Assert.Equal(new ButtonNames(true, "Cross", "Circle", "L1", "R1"), ButtonNames.For("dualsense-edge"));
        Assert.Equal(new ButtonNames(true, "Cross", "Circle", "L1", "R1"), ButtonNames.For("dualshock4"));
        Assert.Equal(new ButtonNames(false, "A", "B", "L", "R"), ButtonNames.For("nintendo"));
        Assert.Equal(new ButtonNames(false, "A", "B", "LB", "RB"), ButtonNames.For("xbox"));
        Assert.Equal(new ButtonNames(false, "A", "B", "LB", "RB"), ButtonNames.For("other"));
    }

    [Fact]
    public void Play_is_reached_and_pressed_with_the_controller() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode") && Ui.Shows(window, "On"), "the two consoles are on the page with their states");
        Assert.Equal(["controllers", "--json", "--watch", "--presses"], fake.Last("controllers"));
        Assert.False(window.FindControl<StackPanel>("Hints")!.IsEffectivelyVisible);   // no controller, nothing to say about its buttons
        fake.Watch!.Say(DualSense);
        Ui.Until(() => window.FindControl<StackPanel>("Hints")!.IsEffectivelyVisible, "the bottom line says what the controller's buttons do");
        Assert.True(Ui.Shows(window, "Choose") && Ui.Shows(window, "Pages") && Ui.Shows(window, "L1") && Ui.Shows(window, "R1"));
        Assert.False(Ui.Shows(window, "Back"));   // on the first page there is nowhere to go back to
        Assert.False(window.FindControl<TextBlock>("AcceptName")!.IsEffectivelyVisible);   // a cross is drawn, not written

        // The first press only shows where the focus is: on the one thing the page is for.
        Assert.False(model.ControllerInUse);
        Press(fake, "accept");
        Assert.True(model.ControllerInUse);
        Assert.Null(fake.Last("play"));
        Assert.Equal("Play", FocusedText(window));
        Assert.Equal("Living Room", FocusedCard(window)!.Name);
        Assert.True(Focused(window)!.Classes.Contains(":focus-visible"), "the ring shows on what has the focus");
        Ui.Capture(window, "controller-home-dark");

        // Along the card, over to the next one, and back.
        Press(fake, "right");
        Assert.Equal("Forget", FocusedText(window));
        Press(fake, "right");
        Assert.Equal("Den", FocusedCard(window)!.Name);
        Assert.Equal("Play", FocusedText(window));
        Press(fake, "right");
        Assert.Equal("Wake", FocusedText(window));
        Press(fake, "up");
        Assert.Contains(FocusedText(window), new[] { "Add a console", "Look again" });
        Press(fake, "down");
        Assert.NotNull(FocusedCard(window));
        while (FocusedCard(window)!.Name != "Living Room" || FocusedText(window) != "Play")
        {
            string before = FocusedText(window) + FocusedCard(window)!.Name;
            Press(fake, "left");
            Assert.NotEqual(before, FocusedText(window) + FocusedCard(window)?.Name);
        }
        // Nothing lies further left: the focus stays where it is.
        Press(fake, "left");
        Assert.Equal("Play", FocusedText(window));
        Assert.Equal("Living Room", FocusedCard(window)!.Name);

        // Play on the second console: not the button the page begins at, so that where the focus
        // is after the session says whether it came back to what was pressed.
        Press(fake, "right");
        Press(fake, "right");
        Assert.Equal("Den", FocusedCard(window)!.Name);
        Assert.Equal("Play", FocusedText(window));
        FakeSession watch = fake.Watch;
        Press(fake, "accept");
        Ui.Until(() => fake.Last("play") is not null, "the program was started for a session");
        Assert.Equal("Den", fake.Last("play")![1]);
        Assert.True(watch.Stopped);
        // While the session runs no Play can be pressed: the focus is on something that can.
        Ui.Until(() => Focused(window) is { IsEffectivelyEnabled: true } && FocusedText(window) != "Play", "the focus left the Play that cannot be pressed now");
        // The stream's window is up and this one out of its way; then the session ends.
        fake.Session!.Say("2026-01-01T00:00:01.000Z INFO  render: first picture shown, 1920x1080");
        Ui.Until(() => window.WindowState == WindowState.Minimized, "the window got out of the stream's way");
        fake.Session.End(0, "Goodbye sent and acknowledged.\n");
        Ui.Until(() => !model.Busy && !ReferenceEquals(fake.Watch, watch), "the session ended and the controllers are watched again");
        // Back on the page, the focus is where a controller goes on from, with its ring.
        Ui.Until(() => window.WindowState == WindowState.Normal && FocusedText(window) == "Play" && FocusedCard(window)?.Name == "Den" && Focused(window)!.Classes.Contains(":focus-visible"),
                 "the focus is on the Play that was pressed again, with its ring");
        window.Close();
    });

    [Fact]
    public void A_press_first_shows_the_ring_on_what_the_pointer_left_the_focus_on() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        fake.Watch!.Say(DualSense);
        Ui.Click(window, window.FindControl<Button>("NavSettings")!);
        Assert.True(model.IsSettings);
        var cards = window.FindControl<ItemsControl>("ThemeCards")!;
        cards.BringIntoView();
        Ui.Settle();
        Button Swatch(string name) => cards.GetVisualDescendantButtons().First(button => (button.DataContext as ThemeChoice)?.Name == name);
        Ui.Click(window, Swatch("Midnight Blue"));
        Assert.Equal("midnight", model.Settings.Theme);
        model.Settings.ThemeChoices.First(choice => choice.Name == "Slate").Pick.Execute(null);
        Assert.Equal("slate", model.Settings.Theme);
        // The pointer left the focus on the look it pressed, with no ring around it.
        Assert.Same(Swatch("Midnight Blue"), Focused(window));
        Assert.DoesNotContain(":focus-visible", Swatch("Midnight Blue").Classes);

        // "Accept" does not press what nobody saw marked: first the ring, and only the next press presses.
        Press(fake, "accept");
        Assert.Equal("slate", model.Settings.Theme);
        Assert.Contains(":focus-visible", Swatch("Midnight Blue").Classes);
        Press(fake, "accept");
        Assert.Equal("midnight", model.Settings.Theme);
        window.Close();
    });

    [Fact]
    public void The_hints_follow_the_controller_that_was_pressed_last() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder, dark: false);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        fake.Watch!.Say(DualSenseAndXbox);
        Ui.Until(() => Ui.Shows(window, "L1"), "the first controller's names are on the bottom line");
        Press(fake, "down", controller: 1);
        Assert.True(Ui.Shows(window, "LB") && Ui.Shows(window, "RB") && !Ui.Shows(window, "L1"));
        Assert.True(window.FindControl<TextBlock>("AcceptName")!.IsEffectivelyVisible);
        Assert.Equal("A", window.FindControl<TextBlock>("AcceptName")!.Text);
        Ui.Capture(window, "controller-home-light");
        Press(fake, "down", controller: 0);
        Assert.True(Ui.Shows(window, "L1") && !Ui.Shows(window, "LB"));
        // A press from a controller the list does not have yet still counts; the names stay the first one's.
        Press(fake, "next", controller: 7);
        Assert.True(model.IsSettings);
        Assert.True(Ui.Shows(window, "L1"));
        // The last controller goes: nothing is said about buttons, and what only a controller needs goes too.
        fake.Watch.Say("""{"controllers": []}""");
        Ui.Until(() => !window.FindControl<StackPanel>("Hints")!.IsEffectivelyVisible, "the hints went with the controller");
        Assert.False(model.ControllerInUse);
        window.Close();
    });

    [Fact]
    public void Presses_count_only_while_the_window_is_in_front() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        fake.Watch!.Say(DualSense);
        Assert.True(window.IsActive);
        // What the desktop tells a window when another program's window comes in front of it.
        Ui.DesktopSays(window, "Deactivated");
        Ui.Settle();
        Assert.False(window.IsActive);
        // Another program has the controller's attention: nothing here moves, and nothing is started.
        Press(fake, "next");
        Press(fake, "accept");
        Press(fake, "accept");
        Assert.True(model.IsHome);
        Assert.Null(fake.Last("play"));
        Assert.False(model.ControllerInUse);   // nor is this app "being used with a controller" for it
        Ui.DesktopSays(window, "Activated");
        Ui.Settle();
        Assert.True(window.IsActive);
        Press(fake, "next");
        Assert.True(model.IsSettings);
        // Out of the way of a stream's window: nothing is pressed in a window nobody sees.
        window.WindowState = WindowState.Minimized;
        Ui.Settle();
        Press(fake, "previous");
        Assert.True(model.IsSettings);
        window.Close();
    });

    [Fact]
    public void Settings_are_changed_with_the_controller() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        fake.Watch!.Say(DualSense);
        Press(fake, "next");
        Assert.True(model.IsSettings);
        Assert.True(Ui.Shows(window, "Back"));
        // The page's first control has the focus without another press.
        Ui.Until(() => Focused(window) is ComboBox, "the focus is on the first setting");
        SettingsViewModel settings = model.Settings;
        Choice first = settings.Resolution;
        Assert.Equal(first, settings.Resolutions[((ComboBox)Focused(window)!).SelectedIndex]);

        // Left and right change a choice; "accept" takes the next one.
        int at = settings.Resolutions.ToList().IndexOf(first);
        Press(fake, at > 0 ? "left" : "right");
        Assert.Equal(settings.Resolutions[at > 0 ? at - 1 : at + 1], settings.Resolution);
        Press(fake, at > 0 ? "right" : "left");
        Assert.Equal(first, settings.Resolution);
        Press(fake, "accept");
        Assert.Equal(settings.Resolutions[(at + 1) % settings.Resolutions.Count], settings.Resolution);

        // Down the page to the cushion's slider, whatever lies between.
        Slider Cushion() => window.GetVisualDescendants().OfType<Slider>().First(slider => slider.Maximum == 50 && slider.TickFrequency == 5);
        for (int i = 0; i < 20 && !ReferenceEquals(Focused(window), Cushion()); ++i) Press(fake, "down");
        Assert.Same(Cushion(), Focused(window));
        Assert.Equal(0, settings.Cushion);
        Press(fake, "right");
        Press(fake, "right");
        Press(fake, "right");
        Press(fake, "left");
        Assert.Equal(10, settings.Cushion);
        Ui.Until(() => Ui.Shows(window, "10 ms of added delay"), "the page says what the cushion costs");
        Assert.Same(Cushion(), Focused(window));
        Ui.Capture(window, "controller-settings-dark");

        // One up is the switch for even pacing: "accept" flips it, and the slider under it can then not be used.
        Press(fake, "up");
        Assert.IsType<ToggleSwitch>(Focused(window));
        Assert.True(settings.Pacing);
        Press(fake, "accept");
        Assert.False(settings.Pacing);
        Assert.False(Cushion().IsEffectivelyEnabled);
        Press(fake, "accept");
        Assert.True(settings.Pacing);

        // The bitrate's box and its slider: both are reached, one above the other.
        for (int i = 0; i < 20 && Focused(window) is not CheckBox { Content: "Automatic" }; ++i) Press(fake, "up");
        Assert.True(settings.AutoBitrate);
        Press(fake, "accept");
        Assert.False(settings.AutoBitrate);
        Press(fake, "down");
        Assert.IsType<Slider>(Focused(window));
        double bitrate = settings.BitrateMbps;
        Press(fake, "right");
        Assert.Equal(bitrate + 1, settings.BitrateMbps);
        Press(fake, "up");
        Assert.IsType<CheckBox>(Focused(window));

        Press(fake, "back");
        Assert.True(model.IsHome);
        Ui.Until(() => File.Exists(settings.File) && new SettingsStore(settings.File).Load() is { CushionMs: 10, BitrateKbps: > 0 }, "what the controller chose is kept");
        window.Close();
    });

    // Every control of the page that is showing, and whether the controller's directions lead to it
    // from where the page begins. A control beside a slider, say, could never be reached: left and
    // right change the slider's value.
    private static void AssertEverythingIsReached(MainWindow window, string pageName)
    {
        Ui.Settle();
        ControllerNavigator navigator = window.Navigator;
        Control root = new[] { "HomeScroll", "SettingsScroll", "DiagnosticsScroll", "AddScroll" }.Select(name => window.FindControl<ScrollViewer>(name)!).First(scroll => scroll.IsVisible);
        List<Control> stops = root.GetVisualDescendants().OfType<Control>()
                                  .Where(control => ControllerNavigator.IsStop(control) && control.Focusable && control.IsEffectivelyVisible && control.IsEffectivelyEnabled).ToList();
        Assert.NotEmpty(stops);
        navigator.FocusFirst();
        Ui.Settle();
        Control? start = navigator.Focused;
        Assert.NotNull(start);
        var reached = new HashSet<Control> { start };
        var queue = new Queue<Control>([start]);
        while (queue.Count > 0)
        {
            Control from = queue.Dequeue();
            foreach (NavigationDirection direction in new[] { NavigationDirection.Up, NavigationDirection.Down, NavigationDirection.Left, NavigationDirection.Right })
            {
                if (ControllerNavigator.Adjusts(from) && direction is NavigationDirection.Left or NavigationDirection.Right) continue;
                // Where the search starts from must be on the screen, as it is when a person gets there.
                from.BringIntoView();
                Ui.Settle();
                if (navigator.Next(from, direction) is { } next && reached.Add(next)) queue.Enqueue(next);
            }
        }
        List<string> missed = stops.Where(stop => !reached.Contains(stop))
                                   .Select(stop => stop.Name ?? (stop as Button)?.Content as string ?? stop.GetType().Name).ToList();
        List<string> beyond = reached.Where(control => !stops.Contains(control))
                                     .Select(control => control.Name ?? (control as Button)?.Content as string ?? control.GetType().Name).ToList();
        Assert.True(missed.Count == 0, $"{pageName}: the controller cannot get to: {string.Join(", ", missed)} ({reached.Count} reached of {stops.Count} on the page" +
                                       (beyond.Count == 0 ? ")" : $"; reached beyond the page: {string.Join(", ", beyond)})"));
    }

    [Fact]
    public void Every_control_of_every_page_is_reached_with_the_directions() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        fake.Answers["discover"] = arguments => arguments.Contains("--host")
            ? new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":true,"supported":true}]""", "")
            : new RunResult(0, """[{"name":"Den","kind":"PS4","state":"standby","address":"192.0.2.11","host_id":"02005E102031","system_version":"1","running_app":"","registered":true,"supported":true},{"name":"Attic","kind":"PS5","state":"ready","address":"192.0.2.12","host_id":"02005E102032","system_version":"1","running_app":"","registered":false,"supported":true}]""", "");
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "In rest mode") && Ui.Shows(window, "On"), "the two consoles are on the page with their states");
        fake.Watch!.Say(DualSense);
        Press(fake, "down");
        AssertEverythingIsReached(window, "the consoles");
        model.Consoles[1].ConfirmForget = true;
        AssertEverythingIsReached(window, "the consoles, one asking whether to forget it");
        model.Consoles[1].ConfirmForget = false;

        Press(fake, "next");
        Assert.True(model.IsSettings);
        AssertEverythingIsReached(window, "the settings");
        model.Settings.AutoBitrate = false;
        AssertEverythingIsReached(window, "the settings with a bitrate of one's own");

        Press(fake, "next");
        Assert.True(model.IsDiagnostics);
        Ui.Until(() => model.Diagnostics.Known, "the diagnostics are there");
        AssertEverythingIsReached(window, "the diagnostics");
        Press(fake, "previous");
        Assert.True(model.IsSettings);

        Press(fake, "back");
        Assert.True(model.IsHome);
        Ui.Click(window, window.FindControl<Button>("AddButton")!);
        Assert.True(model.IsAddConsole);
        Ui.Until(() => Ui.Shows(window, "192.0.2.12"), "the consoles that answered are listed");
        model.AddConsole.Address = "192.0.2.99";
        AssertEverythingIsReached(window, "adding a console, step 1");
        model.AddConsole.Choose.Execute(model.AddConsole.Found.First(found => found.Name == "Attic"));
        Assert.True(model.AddConsole.IsStep2);
        model.AddConsole.AccountId = "1234567890123";
        AssertEverythingIsReached(window, "adding a console, step 2");
        model.AddConsole.OpenBrowser = _ => { };
        model.AddConsole.OpenInBrowser.Execute(null);
        model.AddConsole.Pasted = "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=abc";
        AssertEverythingIsReached(window, "adding a console, step 2 with the browser's address");
        model.AddConsole.Next.Execute(null);
        Assert.True(model.AddConsole.IsStep3);
        model.AddConsole.Pin = "1234 5678";
        AssertEverythingIsReached(window, "adding a console, step 3 with the PIN's keys");
        window.Close();
    });

    [Fact]
    public void A_console_is_added_with_the_controller_alone() => Ui.Run(() =>
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, "[]", "");
        fake.Answers["account"] = _ => new RunResult(0, """{"account_id":"ywT7cR8BAAA="}""", "");
        fake.Answers["discover"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":false,"supported":true}]""", "");
        fake.Answers["register"] = _ => new RunResult(0, "Registered with Living Room (PS5).\n", "");
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder);
        Ui.Until(() => Ui.Shows(window, "No console yet"), "an empty home page says what to do");
        fake.Watch!.Say(DualSense);
        Press(fake, "down");
        Assert.Equal("AddFirst", Focused(window)!.Name);
        Press(fake, "accept");
        Assert.True(model.IsAddConsole);
        Ui.Until(() => Ui.Shows(window, "192.0.2.10"), "the console that answered is listed");

        // The console that answered is where the page begins, though it came after the page did.
        Ui.Until(() => Focused(window) is Button { DataContext: FoundConsole }, "the focus is on the console that answered");
        Ui.Capture(window, "controller-add-dark");
        Press(fake, "accept");
        Assert.True(model.AddConsole.IsStep2);
        Ui.Until(() => model.AddConsole.AccountIdLooksRight, "the account that is already known is filled in");
        Ui.Until(() => Focused(window) is not null, "the focus followed to the second step");
        for (int i = 0; i < 10 && FocusedText(window) != "Next"; ++i) Press(fake, "down");
        Assert.Equal("Next", FocusedText(window));
        Press(fake, "accept");
        Assert.True(model.AddConsole.IsStep3);

        // The PIN's keys show for whoever came here with a controller, and the first of them has the focus.
        var pad = window.FindControl<Avalonia.Controls.Primitives.UniformGrid>("PinPad")!;
        Ui.Until(() => pad.IsEffectivelyVisible && FocusedText(window) == "1", "the PIN's keys are up and the focus is on the 1");
        Assert.False(window.FindControl<Button>("RegisterButton")!.IsEffectivelyEnabled);
        // 1 2 3 / 4 5 6 / 7 8 9 / _ 0 erase: the PIN 1235 8900, with a slip that is erased.
        foreach (string press in new[]
                 {
                     "accept", "right", "accept", "right", "accept",          // 1 2 3
                     "down", "left", "accept",                                // 5
                     "down", "accept", "right", "accept",                     // 8 9
                     "accept",                                                // a 9 too many
                     "down", "accept",                                        // erased
                     "left", "accept", "accept",                              // 0 0
                 })
        {
            Press(fake, press);
        }
        Assert.Equal("1235 8900", model.AddConsole.Pin);
        Assert.Equal("1235 8900", window.FindControl<TextBox>("PinBox")!.Text);
        Press(fake, "accept");   // a ninth digit is not taken
        Assert.Equal("1235 8900", model.AddConsole.Pin);
        Ui.Capture(window, "controller-pin-dark");

        fake.Answers["consoles"] = _ => new RunResult(0, """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"}]""", "");
        Press(fake, "down");
        Assert.Equal("Register", FocusedText(window));
        Press(fake, "accept");
        Ui.Until(() => model.IsHome && Ui.Shows(window, "Living Room is registered. Press Play to start."), "the new console is on the home page");
        Assert.Equal(["register", "--host", "192.0.2.10", "--pin", "12358900", "--account-id", "ywT7cR8BAAA="], fake.Last("register"));
        // And Play is one more press away.
        Ui.Until(() => FocusedText(window) == "Play", "the focus is on the new console's Play");
        window.Close();
    });

    [Fact]
    public void Back_leads_out_step_by_step_and_a_keyboard_puts_the_pins_keys_away() => Ui.Run(() =>
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        (MainWindow window, MainViewModel model) = Ui.Open(fake, _folder, dark: false);
        Ui.Until(() => Ui.Shows(window, "In rest mode"), "the consoles are on the page");
        fake.Watch!.Say(DualSense);
        Press(fake, "down");

        // On the first page: back takes a question away, then the last notice.
        fake.Answers["wake"] = _ => new RunResult(0, "Den is awake.\n", "");
        model.Consoles[0].AskForget.Execute(null);
        model.Wake(model.Consoles[1]);
        Ui.Until(() => !model.Busy && model.HasNotice, "there is a notice on the page");
        Press(fake, "back");
        Assert.False(model.Consoles[0].ConfirmForget);
        Assert.True(model.HasNotice);
        Press(fake, "back");
        Assert.False(model.HasNotice);
        Press(fake, "back");
        Assert.True(model.IsHome);

        Ui.Click(window, window.FindControl<Button>("AddButton")!);
        Ui.Until(() => Ui.Shows(window, "192.0.2.11"), "the console that answered is listed");
        model.AddConsole.Choose.Execute(model.AddConsole.Found[0]);
        model.AddConsole.AccountId = "1234567890123";
        model.AddConsole.Next.Execute(null);
        Assert.True(model.AddConsole.IsStep3);
        var pad = window.FindControl<Avalonia.Controls.Primitives.UniformGrid>("PinPad")!;
        Ui.Until(() => pad.IsEffectivelyVisible, "the PIN's keys are up");
        Ui.Capture(window, "controller-pin-light");

        // Whoever types has a keyboard: the keys on the page get out of the way.
        TextBox box = window.FindControl<TextBox>("PinBox")!;
        Ui.Click(window, box);
        window.KeyPressQwerty(PhysicalKey.Digit4, RawInputModifiers.None);
        window.KeyReleaseQwerty(PhysicalKey.Digit4, RawInputModifiers.None);
        Ui.Settle();
        Assert.False(model.ControllerInUse);
        Assert.False(pad.IsEffectivelyVisible);
        // And the next press of the controller brings them back.
        Press(fake, "down");
        Assert.True(pad.IsEffectivelyVisible);

        Press(fake, "back");
        Assert.True(model.AddConsole.IsStep2);
        Press(fake, "back");
        Assert.True(model.AddConsole.IsStep1);
        Press(fake, "back");
        Assert.True(model.IsHome);
        Press(fake, "previous");
        Assert.True(model.IsHome);
        // The pages in their order at the top: consoles, settings, diagnostics; past the ends, nowhere.
        Press(fake, "next");
        Press(fake, "next");
        Assert.True(model.IsDiagnostics);
        Press(fake, "next");
        Assert.True(model.IsDiagnostics);
        Press(fake, "previous");
        Assert.True(model.IsSettings);
        Press(fake, "previous");
        Assert.True(model.IsHome);
        window.Close();
    });
}
