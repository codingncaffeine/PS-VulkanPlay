using System.Buffers.Binary;
using System.Text;
using PSVulkanPlay.Core;
using PSVulkanPlay.ViewModels;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>Consoles in Steam's library: its binary list, the entries and pictures the app writes, always in a stand-in Steam folder.</summary>
public sealed class SteamTests : IDisposable
{
    private readonly string _folder = Directory.CreateTempSubdirectory("psvp-steam-").FullName;
    private readonly RegisteredConsole _living = new("Living Room", "PS5", "02:00:5e:10:20:30", "192.0.2.10");

    public void Dispose() => Directory.Delete(_folder, true);

    /// <summary>A Steam folder with accounts (the last one used last), as Steam leaves it.</summary>
    private string Root(params string[] accounts)
    {
        string root = Path.Combine(_folder, "Steam");
        DateTime when = DateTime.UtcNow.AddHours(-accounts.Length);
        foreach (string account in accounts)
        {
            string config = Path.Combine(root, "userdata", account, "config");
            Directory.CreateDirectory(config);
            File.WriteAllText(Path.Combine(config, "localconfig.vdf"), "\"UserLocalConfigStore\"\n{\n}\n");
            File.SetLastWriteTimeUtc(Path.Combine(config, "localconfig.vdf"), when = when.AddMinutes(30));
        }
        return root;
    }

    private SteamShortcuts Steam(string root, bool running = false) =>
        new(root, "/opt/PS VulkanPlay/ps-vulkanplay", () => running, SteamShortcuts.Resource("banner.png"), SteamShortcuts.Resource("icon.png"));

    private static (int Width, int Height) PngSize(string file)
    {
        byte[] head = File.ReadAllBytes(file)[..24];
        Assert.Equal("\x89PNG", Encoding.Latin1.GetString(head, 0, 4));
        return (BinaryPrimitives.ReadInt32BigEndian(head.AsSpan(16)), BinaryPrimitives.ReadInt32BigEndian(head.AsSpan(20)));
    }

    /// <summary>A list as Steam writes it when a game was added by hand.</summary>
    private static VdfMap SteamsOwnList()
    {
        var entry = new VdfMap();
        entry.Add("appid", 0xb25f120bu);
        entry.Add("appname", "Some Emulator");
        entry.Add("Exe", "\"/usr/bin/emulator\"");
        entry.Add("StartDir", "\"/usr/bin/\"");
        entry.Add("LaunchOptions", "");
        entry.Add("tags", new VdfMap());
        var list = new VdfMap();
        list.Add("0", entry);
        var root = new VdfMap();
        root.Add("shortcuts", list);
        return root;
    }

    [Fact]
    public void The_binary_list_reads_back_as_it_was_written()
    {
        byte[] bytes = BinaryVdf.Write(SteamsOwnList());
        // Type, key, value ... 0x08 at the end of every map: the entry's, the list's and the file's.
        Assert.Equal(new byte[] { 0x00, (byte)'s' }, bytes[..2]);
        Assert.Equal(new byte[] { 0x08, 0x08, 0x08, 0x08 }, bytes[^4..]);
        VdfMap again = BinaryVdf.Parse(bytes);
        Assert.Equal(bytes, BinaryVdf.Write(again));
        var entry = (VdfMap)((VdfMap)again.Get("shortcuts")!).Get("0")!;
        Assert.Equal(0xb25f120bu, entry.Get("APPID"));
        Assert.Equal("Some Emulator", entry.GetString("AppName"));
        Assert.Throws<FormatException>(() => BinaryVdf.Parse(bytes[..^5]));
        Assert.Throws<FormatException>(() => BinaryVdf.Parse([0x07, (byte)'x', 0]));
    }

    [Fact]
    public void A_shortcut_number_is_the_crc_of_what_it_starts_and_its_name_with_the_top_bit()
    {
        Assert.Equal(0xCBF43926u, SteamShortcuts.Crc32(Encoding.ASCII.GetBytes("123456789")));
        uint id = SteamShortcuts.ShortcutId("\"/usr/bin/ps-vulkanplay\"", "Living Room (PS VulkanPlay)");
        Assert.Equal(0x80000000u, id & 0x80000000u);
        Assert.Equal(id, SteamShortcuts.Crc32(Encoding.UTF8.GetBytes("\"/usr/bin/ps-vulkanplay\"Living Room (PS VulkanPlay)")) | 0x80000000u);
    }

    [Fact]
    public void Adding_writes_the_entry_and_its_pictures_into_the_account_used_last()
    {
        string root = Root("1111", "0", "2222");
        SteamShortcuts steam = Steam(root);
        Assert.Equal(Path.Combine(root, "userdata", "2222"), steam.Account());
        Assert.False(steam.Has(_living));
        Assert.Equal("", steam.Add(_living));
        Assert.True(steam.Has(_living));
        string file = Path.Combine(root, "userdata", "2222", "config", "shortcuts.vdf");
        Assert.False(File.Exists(Path.Combine(root, "userdata", "1111", "config", "shortcuts.vdf")));
        var list = (VdfMap)BinaryVdf.Parse(File.ReadAllBytes(file)).Get("shortcuts")!;
        var entry = (VdfMap)Assert.Single(list.Items).Value;
        Assert.Equal("0", list.Items[0].Key);
        Assert.Equal("\"/opt/PS VulkanPlay/ps-vulkanplay\"", entry.GetString("Exe"));
        Assert.Equal("\"/opt/PS VulkanPlay/\"", entry.GetString("StartDir"));
        Assert.Equal("Living Room (PS VulkanPlay)", entry.GetString("appname"));
        Assert.Equal("--play 02:00:5e:10:20:30", entry.GetString("LaunchOptions"));
        uint id = (uint)entry.Get("appid")!;
        Assert.Equal(SteamShortcuts.ShortcutId(entry.GetString("Exe"), entry.GetString("appname")), id);
        string grid = Path.Combine(root, "userdata", "2222", "config", "grid");
        Assert.Equal((600, 900), PngSize(Path.Combine(grid, $"{id}p.png")));
        Assert.Equal((920, 430), PngSize(Path.Combine(grid, $"{id}.png")));
        Assert.Equal((1920, 620), PngSize(Path.Combine(grid, $"{id}_hero.png")));
        Assert.Equal((256, 256), PngSize(Path.Combine(grid, $"{id}_logo.png")));
        Assert.Equal(Path.Combine(grid, $"{id}_icon.png"), entry.GetString("icon"));
        // PSVP_SHOTS: the pictures too, to look at.
        if (Environment.GetEnvironmentVariable("PSVP_SHOTS") is { Length: > 0 } shots)
        {
            Directory.CreateDirectory(shots);
            foreach (string suffix in new[] { "p", "", "_hero" }) File.Copy(Path.Combine(grid, $"{id}{suffix}.png"), Path.Combine(shots, $"steam{(suffix.Length == 0 ? "-wide" : suffix.Replace('_', '-'))}.png"), true);
        }
        Assert.True(File.Exists(entry.GetString("icon")));
        // Nothing was there to keep a copy of.
        Assert.False(File.Exists(file + ".before-psvulkanplay"));
    }

    [Fact]
    public void Steams_own_entries_stay_adding_again_replaces_and_removing_renumbers()
    {
        string root = Root("2222");
        string file = Path.Combine(root, "userdata", "2222", "config", "shortcuts.vdf");
        byte[] steams = BinaryVdf.Write(SteamsOwnList());
        File.WriteAllBytes(file, steams);
        SteamShortcuts steam = Steam(root);
        Assert.Equal("", steam.Add(_living));
        Assert.Equal("", steam.Add(_living));
        var list = (VdfMap)BinaryVdf.Parse(File.ReadAllBytes(file)).Get("shortcuts")!;
        Assert.Equal(["0", "1"], list.Items.Select(item => item.Key));
        Assert.Equal("Some Emulator", ((VdfMap)list.Items[0].Value).GetString("appname"));
        // Steam's file as it was before the first change, kept once.
        Assert.Equal(steams, File.ReadAllBytes(file + ".before-psvulkanplay"));
        uint id = (uint)((VdfMap)list.Items[1].Value).Get("appid")!;
        Assert.Equal("", steam.Remove(_living));
        Assert.False(steam.Has(_living));
        Assert.Equal(steams, File.ReadAllBytes(file));
        Assert.False(File.Exists(Path.Combine(root, "userdata", "2222", "config", "grid", $"{id}p.png")));
    }

    [Fact]
    public void Nothing_is_written_while_Steam_runs_or_into_a_list_that_cannot_be_read()
    {
        string root = Root("2222");
        string file = Path.Combine(root, "userdata", "2222", "config", "shortcuts.vdf");
        Assert.Contains("Close Steam", Steam(root, running: true).Add(_living));
        Assert.False(File.Exists(file));
        File.WriteAllBytes(file, [0x00, (byte)'s', 0, 0x05]);
        Assert.Contains("could not be read, so nothing was changed", Steam(root).Add(_living));
        Assert.Equal(new byte[] { 0x00, (byte)'s', 0, 0x05 }, File.ReadAllBytes(file));
        Assert.Contains("no account", Steam(Path.Combine(_folder, "elsewhere")).Add(_living));
    }

    [Fact]
    public async Task The_card_offers_Steam_only_where_it_is_installed()
    {
        FakeRunner fake = FakeRunner.WithTwoConsoles();
        var store = new SettingsStore(Path.Combine(_folder, "app.json"));
        var without = new MainViewModel(fake, store, action => action());
        await without.RefreshAsync();
        Assert.False(without.Consoles.First().HasSteam);

        string root = Root("2222");
        var model = new MainViewModel(fake, store, action => action(), null, null, Steam(root));
        await model.RefreshAsync();
        ConsoleCard card = model.Consoles.First(c => c.Name == "Living Room");
        Assert.True(card.HasSteam);
        Assert.Equal("Add to Steam", card.SteamText);
        card.ToggleSteam.Execute(null);
        Assert.True(card.InSteam);
        Assert.Equal("Remove from Steam", card.SteamText);
        Assert.Contains("is in Steam's library now", model.Notice);
        card.ToggleSteam.Execute(null);
        Assert.False(card.InSteam);
    }
}
