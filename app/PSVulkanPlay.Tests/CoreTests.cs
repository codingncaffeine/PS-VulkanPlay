using PSVulkanPlay.Core;
using Xunit;

namespace PSVulkanPlay.Tests;

public class CoreTests
{
    [Fact]
    public void Registered_consoles_are_read_from_the_programs_list()
    {
        var consoles = ConsoleJson.Registered(FakeRunner.TwoConsoles);
        Assert.Equal(2, consoles.Count);
        Assert.Equal(new RegisteredConsole("Living Room", "PS5", "02:00:5e:10:20:30", "192.0.2.10"), consoles[0]);
        Assert.Equal("02005e102030", consoles[0].HostId);
        Assert.Equal("", consoles[1].LastAddress);
    }

    [Fact]
    public void Found_consoles_are_read_with_their_state()
    {
        var found = ConsoleJson.Found("""[{"name":"Living Room","kind":"PS5","state":"standby","address":"192.0.2.10","host_id":"02005E102030","system_version":"09.00","running_app":"","registered":false,"supported":true}]""");
        FoundConsole console = Assert.Single(found);
        Assert.True(console.Resting);
        Assert.False(console.Ready);
        Assert.False(console.Registered);
        Assert.True(console.Supported);
        Assert.Equal("02005e102030", console.HostId);
    }

    [Theory]
    [InlineData("")]
    [InlineData("No console answered.")]
    [InlineData("{\"name\":1}")]
    [InlineData("[1, \"two\", null]")]
    [InlineData("[{\"name\":")]
    public void What_is_not_a_list_of_consoles_is_an_empty_list(string text)
    {
        Assert.Empty(ConsoleJson.Found(text));
        Assert.Empty(ConsoleJson.Registered(text));
    }

    [Fact]
    public void The_account_id_is_read_and_may_be_missing()
    {
        Assert.Equal("ywT7cR8BAAA=", ConsoleJson.AccountId("""{"account_id":"ywT7cR8BAAA="}"""));
        Assert.Equal("", ConsoleJson.AccountId("""{"account_id":""}"""));
        Assert.Equal("", ConsoleJson.AccountId("nothing"));
    }

    [Fact]
    public void A_first_runs_settings_ask_for_what_the_program_does_by_itself_and_to_wake()
    {
        Assert.Equal(["play", "Living Room", "--host", "192.0.2.10", "--wake", "--resolution", "1080", "--fps", "60", "--codec", "hevc", "--controller", "auto", "--haptics-volume", "100"],
                     new AppSettings().PlayArguments("Living Room", "192.0.2.10"));
    }

    [Fact]
    public void A_file_that_set_a_brightness_before_measuring_existed_keeps_it()
    {
        Assert.Equal(true, new AppSettings().Valid().HdrPeakMeasured);
        Assert.Equal(false, new AppSettings { HdrPeakNits = 1500 }.Valid().HdrPeakMeasured);
        Assert.Contains("--hdr-peak", new AppSettings { HdrPeakNits = 1500 }.PlayArguments("Den", ""));
        Assert.DoesNotContain("--hdr-peak", new AppSettings().PlayArguments("Den", ""));
        // A brightness chosen with measuring off goes along even when it is 1000.
        Assert.Equal(["--hdr-peak", "1000"], new AppSettings { HdrPeakMeasured = false }.PlayArguments("Den", "").SkipWhile(a => a != "--hdr-peak").Take(2));
    }

    [Fact]
    public void Controller_settings_the_program_would_refuse_fall_back_to_their_defaults()
    {
        AppSettings valid = new AppSettings { LightBar = "blue", DeadZone = 99, FnLeft = "screenshot", FnRight = "menu" }.Valid();
        Assert.Equal("console", valid.LightBar);
        Assert.Equal(AppSettings.MostDeadZone, valid.DeadZone);
        Assert.Equal("none", valid.FnLeft);
        Assert.Equal("menu", valid.FnRight);
        Assert.Equal("00ff7f", new AppSettings { LightBar = "00FF7F" }.Valid().LightBar);
    }

    [Fact]
    public void Every_setting_becomes_its_option()
    {
        var settings = new AppSettings
        {
            Resolution = 720, FramesPerSecond = 30, Codec = "hdr", BitrateKbps = 20000, CushionMs = 20, Controller = "dualsense",
            HapticsVolume = 60, BackLeft = "cross", BackRight = "r3", WakeFirst = false, RestAfter = true, LossCeiling = 100, ShowStats = true, GpuDecoding = false,
            Reconnect = false, HdrPeakNits = 600, HdrOutput = false, Scaler = "catmull-rom", Deband = false, TriggerEffects = false, LightBar = "FF6A00", PlayerLights = false,
            Motion = false, Touchpad = false, DeadZone = 12, FnLeft = "menu", FnRight = "stats",
        };
        Assert.Equal(["play", "Den", "--resolution", "720", "--fps", "30", "--codec", "hdr", "--bitrate", "20000", "--decoder", "software", "--reconnect", "0", "--controller", "dualsense", "--cushion", "20",
                      "--loss-ceiling", "100", "--hdr-peak", "600", "--hdr-output", "off", "--scaler", "catmull-rom", "--deband", "off", "--haptics-volume", "60", "--back-left", "cross", "--back-right", "r3", "--trigger-effects", "off", "--light-bar", "ff6a00", "--player-lights", "off", "--motion", "off",
                      "--touchpad", "off", "--dead-zone", "12", "--fn-left", "menu", "--fn-right", "stats", "--stats", "--rest"],
                     settings.PlayArguments("Den", ""));
    }

    [Fact]
    public void A_console_with_a_picture_of_its_own_plays_with_it_and_the_others_with_the_apps()
    {
        var settings = new AppSettings { Resolution = 1080, Codec = "hevc" };
        settings.PerConsole["Den"] = new ConsolePicture { Resolution = 720, FramesPerSecond = 30, Codec = "h264", BitrateKbps = 10000 };
        settings.PerConsole[""] = new ConsolePicture { Resolution = 360 };          // no console's: left out
        settings.PerConsole["Attic"] = new ConsolePicture { Resolution = 999, Codec = "vp9" };  // made valid
        IReadOnlyList<string> den = settings.PlayArguments("Den", "");
        Assert.Equal(["720", "30", "h264", "10000"], [Option(den, "--resolution"), Option(den, "--fps"), Option(den, "--codec"), Option(den, "--bitrate")]);
        IReadOnlyList<string> living = settings.PlayArguments("Living Room", "");
        Assert.Equal(["1080", "60", "hevc"], [Option(living, "--resolution"), Option(living, "--fps"), Option(living, "--codec")]);
        Assert.DoesNotContain("--bitrate", living);
        AppSettings valid = settings.Valid();
        Assert.Equal(["Attic", "Den"], valid.PerConsole.Keys.Order());
        Assert.Equal(1080, valid.PerConsole["Attic"].Resolution);
        Assert.Equal("hevc", valid.PerConsole["Attic"].Codec);

        // Kept in the file and read back.
        string folder = Path.Combine(Path.GetTempPath(), "psvp-per-console-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new SettingsStore(Path.Combine(folder, "app.json"));
            Assert.Equal("", store.Save(settings));
            AppSettings read = store.Load();
            Assert.Equal(30, read.PerConsole["Den"].FramesPerSecond);
            Assert.Equal("h264", read.PerConsole["Den"].Codec);
        }
        finally
        {
            Directory.Delete(folder, true);
        }
    }

    private static string Option(IReadOnlyList<string> arguments, string name) => arguments[arguments.ToList().IndexOf(name) + 1];

    [Fact]
    public void A_ps4_is_not_told_a_codec_and_pacing_off_takes_the_cushion_with_it()
    {
        var settings = new AppSettings { Pacing = false, CushionMs = 30 };
        IReadOnlyList<string> arguments = settings.PlayArguments("Den", "", ps4: true);
        Assert.DoesNotContain("--codec", arguments);
        Assert.DoesNotContain("--cushion", arguments);
        Assert.Equal("off", arguments[arguments.ToList().IndexOf("--pacing") + 1]);
    }

    [Fact]
    public void Values_the_program_would_refuse_never_reach_it()
    {
        var settings = new AppSettings { Resolution = 1440, FramesPerSecond = 120, Codec = "av1", BitrateKbps = 5, CushionMs = 400, Controller = "wheel", HapticsVolume = -3, BackLeft = "lever", Theme = "pink", LossCeiling = 250 }.Valid();
        Assert.Equal((1080, 60, "hevc", 500, 50, "auto", 0, "", "system", 100),
                     (settings.Resolution, settings.FramesPerSecond, settings.Codec, settings.BitrateKbps, settings.CushionMs, settings.Controller, settings.HapticsVolume, settings.BackLeft, settings.Theme, settings.LossCeiling));
        Assert.Equal(0, new AppSettings { LossCeiling = -1 }.Valid().LossCeiling);
        Assert.Equal((400, 10000), (new AppSettings { HdrPeakNits = 50 }.Valid().HdrPeakNits, new AppSettings { HdrPeakNits = 20000 }.Valid().HdrPeakNits));
    }

    [Fact]
    public void Settings_are_kept_and_read_back_and_a_broken_file_is_a_first_run()
    {
        string folder = Path.Combine(Path.GetTempPath(), "psvp-settings-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new SettingsStore(Path.Combine(folder, "deeper", "app.json"));
            Assert.Equal(1080, store.Load().Resolution);   // no file yet
            Assert.Equal("", store.Save(new AppSettings { Resolution = 540, CushionMs = 25, BackRight = "circle", Theme = "light" }));
            AppSettings read = store.Load();
            Assert.Equal((540, 25, "circle", "light"), (read.Resolution, read.CushionMs, read.BackRight, read.Theme));
            File.WriteAllText(store.File, "{ \"resolution\": ");
            Assert.Equal(1080, store.Load().Resolution);
            File.WriteAllText(store.File, "{ \"resolution\": 99999, \"cushionMs\": 20 }");
            Assert.Equal((1080, 20), (store.Load().Resolution, store.Load().CushionMs));
            // A file of an earlier version has no loss ceiling: it gets the default one.
            Assert.Equal(AppSettings.DefaultLossCeiling, store.Load().LossCeiling);
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, true);
        }
    }

    [Fact]
    public async Task A_console_with_a_known_address_is_asked_there_and_the_others_are_looked_for()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        var directory = new ConsoleDirectory(fake);
        var (consoles, error) = await directory.RegisteredAsync(TestContext.Current.CancellationToken);
        Assert.Equal("", error);
        var status = await directory.StatusAsync(consoles, TestContext.Current.CancellationToken);
        Assert.Equal(ConsoleState.Ready, status[0].State);
        Assert.Equal(ConsoleState.Resting, status[1].State);
        Assert.Equal("192.0.2.11", status[1].Address);   // where it answered from
        var asked = fake.Calls.Where(call => call[0] == "discover").ToList();
        Assert.Equal(2, asked.Count);
        Assert.Contains(asked, call => call.Contains("--host") && call.Contains("192.0.2.10"));
        Assert.Contains(asked, call => !call.Contains("--host"));
    }

    [Fact]
    public async Task A_console_that_does_not_answer_is_not_found_and_keeps_its_last_address()
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, FakeRunner.TwoConsoles, "");
        fake.Answers["discover"] = _ => new RunResult(1, "[]", "No console answered at 192.0.2.10.\n");
        var directory = new ConsoleDirectory(fake);
        var (consoles, _) = await directory.RegisteredAsync(TestContext.Current.CancellationToken);
        var status = await directory.StatusAsync(consoles, TestContext.Current.CancellationToken);
        Assert.All(status, one => Assert.Equal(ConsoleState.NotFound, one.State));
        Assert.Equal("192.0.2.10", status[0].Address);
    }

    [Fact]
    public void Controllers_are_read_from_the_programs_line_and_said_in_one()
    {
        var list = ControllerJson.Read("""{"controllers": [{"name": "DualSense Edge Wireless Controller", "kind": "dualsense-edge", "link": "cable", "battery": 55, "power": "charging", "touchpad": true, "motion": true, "rumble": true, "haptics": "sound-device"}, {"name": "", "kind": "other", "link": "", "battery": -1, "power": "", "haptics": "none"}]}""");
        Assert.NotNull(list);
        Assert.Equal(2, list.Count);
        Assert.Equal("DualSense Edge Wireless Controller  ·  on a cable  ·  55%, charging", list[0].Line);
        Assert.True(list[0].IsDualSense);
        Assert.Equal("Haptics and trigger effects", list[0].Abilities);
        Assert.Equal("A controller", list[1].Line);
        Assert.Equal("", list[1].Abilities);
        Assert.False(list[1].BatteryLow);
        Assert.True(new ControllerInfo("Pad", "dualsense", "wireless", 15, "battery", "rumble").BatteryLow);
        Assert.False(new ControllerInfo("Pad", "dualsense", "cable", 15, "charging", "rumble").BatteryLow);
        Assert.Contains("need the cable", new ControllerInfo("Pad", "dualsense", "wireless", 80, "battery", "rumble").Abilities);
        // A line that is not the list is nothing: a log line must not empty the page.
        Assert.Null(ControllerJson.Read("2026-01-01T00:00:00.000Z WARN  something"));
        Assert.Null(ControllerJson.Read("[]"));
        Assert.Empty(ControllerJson.Read("""{"controllers": []}""")!);
    }

    [Fact]
    public void The_programs_reason_is_what_it_wrote_for_a_person()
    {
        Assert.Equal("No registered console is called 'X'.", new RunResult(1, "", "No registered console is called 'X'.\n").Reason);
        Assert.Equal("said on the other stream", new RunResult(1, "said on the other stream\n", "").Reason);
        Assert.Contains("code 3", new RunResult(3, "", "").Reason);
    }
}
