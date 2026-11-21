using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>Straight into a session from a shortcut or a link, and the shortcuts the app writes for that.</summary>
public sealed class QuickPlayTests : IDisposable
{
    private readonly string _folder = Directory.CreateTempSubdirectory("psvp-quick-").FullName;

    public void Dispose() => Directory.Delete(_folder, true);

    private static async Task Until(Func<bool> met)
    {
        var limit = DateTime.UtcNow.AddSeconds(10);
        while (!met())
        {
            Assert.True(DateTime.UtcNow < limit, "it did not happen");
            await Task.Delay(5);
        }
    }

    [Theory]
    [InlineData(new string[0], null, false)]
    [InlineData(new[] { "--play", "Living Room" }, "Living Room", false)]
    [InlineData(new[] { "--play", "02:00:5e:10:20:30" }, "02:00:5e:10:20:30", false)]
    [InlineData(new[] { "--play" }, null, true)]
    [InlineData(new[] { "--play", " " }, null, true)]
    [InlineData(new[] { "--play", "a", "b" }, null, true)]
    [InlineData(new[] { "ps-vulkanplay://play/Living%20Room" }, "Living Room", false)]
    [InlineData(new[] { "PS-VulkanPlay://PLAY/Den/" }, "Den", false)]
    [InlineData(new[] { "ps-vulkanplay://wake/Den" }, null, true)]
    [InlineData(new[] { "ps-vulkanplay://play/" }, null, true)]
    [InlineData(new[] { "ps-vulkanplay://play/a/b" }, null, true)]
    [InlineData(new[] { "ps-vulkanplay://play/Den%0A--rest" }, null, true)]
    [InlineData(new[] { "--some-avalonia-option" }, null, false)]
    public void The_apps_arguments_say_which_console_to_play_on(string[] args, string? console, bool wrong)
    {
        Assert.Equal(console, QuickPlay.ConsoleAskedFor(args, out string error));
        Assert.Equal(wrong, error.Length > 0);
    }

    [Fact]
    public void A_console_is_found_by_its_name_or_its_hardware_address()
    {
        var registered = ConsoleJson.Registered(FakeRunner.TwoConsoles);
        Assert.Equal("Living Room", QuickPlay.Find(registered, "Living Room")?.Name);
        Assert.Equal("Living Room", QuickPlay.Find(registered, "living room")?.Name);
        Assert.Equal("Den", QuickPlay.Find(registered, "02:00:5E:10:20:31")?.Name);
        Assert.Equal("Den", QuickPlay.Find(registered, "02005e102031")?.Name);
        Assert.Null(QuickPlay.Find(registered, "Attic"));
        Assert.Null(QuickPlay.Find(registered, "02005e10203"));   // eleven digits: no address
    }

    [Fact]
    public async Task A_session_gets_the_arguments_the_Play_button_would_give()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        var settings = new AppSettings { Resolution = 720, ShowStats = true };
        var said = new List<string>();
        Task<int> running = QuickPlay.RunAsync(fake, settings, "02:00:5e:10:20:31", said.Add, TestContext.Current.CancellationToken);
        await Until(() => fake.Session is not null);
        fake.Session!.End(3);
        Assert.Equal(3, await running);
        // The address it was last seen at is the program's default: no --host; a PS4: no codec.
        Assert.Equal(settings.PlayArguments("Den", "", ps4: true), fake.Last("play"));
        Assert.DoesNotContain("--host", fake.Last("play")!);
        Assert.Contains("--stats", fake.Last("play")!);
    }

    [Fact]
    public async Task A_console_that_is_not_registered_starts_nothing()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        var said = new List<string>();
        Assert.Equal(1, await QuickPlay.RunAsync(fake, new AppSettings(), "Attic", said.Add, TestContext.Current.CancellationToken));
        Assert.Null(fake.Session);
        Assert.Contains("No console called Attic is registered here", Assert.Single(said));
    }

    [Theory]
    [InlineData("ps-vulkanplay", "ps-vulkanplay")]
    [InlineData("/usr/bin/ps-vulkanplay", "/usr/bin/ps-vulkanplay")]
    [InlineData("/media/My Games/bin/ps-vulkanplay", "\"/media/My Games/bin/ps-vulkanplay\"")]
    [InlineData("/a/50%/b", "/a/50%%/b")]
    [InlineData("/a/$HOME `x` \"q\" \\b", "\"/a/\\$HOME \\`x\\` \\\"q\\\" \\\\b\"")]
    [InlineData("", "\"\"")]
    public void Exec_arguments_are_quoted_as_the_desktop_entry_specification_says(string argument, string expected) =>
        Assert.Equal(expected, Shortcuts.ExecArgument(argument));

    [Fact]
    public void A_console_shortcut_starts_a_session_by_its_hardware_address()
    {
        var shortcuts = new Shortcuts(Path.Combine(_folder, "applications"), "/media/My Games/bin/ps-vulkanplay");
        var console = new RegisteredConsole("Kid's \\ Room\nTwo", "PS5", "02:00:5e:10:20:30", "192.0.2.10");
        Assert.False(shortcuts.Has(console));
        Assert.Equal("", shortcuts.Add(console));
        Assert.True(shortcuts.Has(console));
        string file = shortcuts.FileFor(console);
        Assert.Equal("io.github.codingncaffeine.PSVulkanPlay.Console_02005e102030.desktop", Path.GetFileName(file));
        string[] lines = File.ReadAllLines(file);
        Assert.Equal("[Desktop Entry]", lines[0]);
        Assert.Contains("Type=Application", lines);
        // The backslash of the name escaped, its line break written as \n: one line each.
        Assert.Contains(@"Name=Kid's \\ Room\nTwo (PS VulkanPlay)", lines);
        // The quotes' backslashes are escaped once more as a string value.
        Assert.Contains("Exec=\"/media/My Games/bin/ps-vulkanplay\" --play 02:00:5e:10:20:30", lines);
        Assert.Contains("Icon=io.github.codingncaffeine.PSVulkanPlay", lines);
        Assert.Contains("Categories=Game;", lines);
        Assert.Equal("", shortcuts.Remove(console));
        Assert.False(shortcuts.Has(console));
        Assert.Equal("", shortcuts.Remove(console));   // one that is not there: nothing to say
    }

    [Fact]
    public void The_written_entry_is_valid_where_the_validator_is_installed()
    {
        string? validator = (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':')
            .Select(dir => Path.Combine(dir, "desktop-file-validate")).FirstOrDefault(File.Exists);
        if (validator is null) return;   // the CI images may not have it
        var shortcuts = new Shortcuts(_folder, "/opt/PS VulkanPlay/ps-vulkanplay");
        var console = new RegisteredConsole("Living Room", "PS5", "02:00:5e:10:20:30", "");
        Assert.Equal("", shortcuts.Add(console));
        var run = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(validator, shortcuts.FileFor(console))
        {
            RedirectStandardOutput = true, RedirectStandardError = true,
        })!;
        string said = run.StandardOutput.ReadToEnd() + run.StandardError.ReadToEnd();
        run.WaitForExit();
        Assert.True(run.ExitCode == 0 && said.Trim().Length == 0, said);
    }

    [Fact]
    public async Task The_card_adds_and_removes_its_shortcut_and_forgetting_removes_it()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        var shortcuts = new Shortcuts(Path.Combine(_folder, "applications"), "ps-vulkanplay");
        var model = new MainViewModel(fake, new SettingsStore(Path.Combine(_folder, "app.json")), action => action(), null, shortcuts);
        await model.RefreshAsync();
        ConsoleCard card = model.Consoles.First(c => c.Name == "Living Room");
        Assert.False(card.HasShortcut);
        Assert.Equal("Add to the app menu", card.ShortcutText);
        card.ToggleShortcut.Execute(null);
        Assert.True(card.HasShortcut);
        Assert.True(File.Exists(shortcuts.FileFor(card.Console)));
        Assert.Equal("Remove from the app menu", card.ShortcutText);
        Assert.Contains("is in the application menu now", model.Notice);
        card.ToggleShortcut.Execute(null);
        Assert.False(File.Exists(shortcuts.FileFor(card.Console)));
        card.ToggleShortcut.Execute(null);
        fake.Answers["forget"] = _ => new RunResult(0, "", "");
        card.Forget.Execute(null);
        await Until(() => !File.Exists(shortcuts.FileFor(card.Console)));
    }

    [Fact]
    public void A_model_with_no_shortcuts_given_keeps_them_beside_its_settings_never_in_the_desktops_folder()
    {
        var model = new MainViewModel(FakeRunner.WithTwoConsoles(), new SettingsStore(Path.Combine(_folder, "app.json")), action => action());
        Assert.Equal(Path.Combine(_folder, "applications"), model.Shortcuts.Folder);
    }
}
