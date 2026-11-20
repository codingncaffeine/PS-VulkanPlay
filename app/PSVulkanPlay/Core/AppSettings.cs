using System.Text.Json;
using System.Text.Json.Serialization;

namespace PSVulkanPlay.Core;

/// <summary>What the user has chosen. Every value is one the stream program takes as an option.</summary>
public sealed class AppSettings
{
    public int Resolution { get; set; } = 1080;         // 360, 540, 720, 1080
    public int FramesPerSecond { get; set; } = 60;      // 30, 60
    public string Codec { get; set; } = "hevc";         // h264, hevc, hdr
    public int BitrateKbps { get; set; }                // 0: the program's own choice
    public bool Pacing { get; set; } = true;
    public int CushionMs { get; set; }                  // 0 to 50
    public string Controller { get; set; } = "auto";    // auto, dualsense, dualshock4
    public int HapticsVolume { get; set; } = 100;       // 0 to 100
    public string BackLeft { get; set; } = "";          // a button's name, or nothing
    public string BackRight { get; set; } = "";
    public bool WakeFirst { get; set; } = true;
    public bool RestAfter { get; set; }
    public string Theme { get; set; } = "system";       // "system", or a look's id (Theming/Themes.cs)

    public static readonly int[] Resolutions = [360, 540, 720, 1080];
    public static readonly int[] FrameRates = [30, 60];
    public static readonly string[] Codecs = ["h264", "hevc", "hdr"];
    public static readonly string[] Controllers = ["auto", "dualsense", "dualshock4"];

    /// <summary>The console's buttons a back button can be, by the names the program takes; the first is "nothing".</summary>
    public static readonly string[] Buttons =
        ["", "cross", "circle", "square", "triangle", "l1", "r1", "l3", "r3", "up", "down", "left", "right", "options", "share", "touchpad", "ps"];

    /// <summary>The same settings with every value inside what the program takes.</summary>
    public AppSettings Valid()
    {
        return new AppSettings
        {
            Resolution = Resolutions.Contains(Resolution) ? Resolution : 1080,
            FramesPerSecond = FrameRates.Contains(FramesPerSecond) ? FramesPerSecond : 60,
            Codec = Codecs.Contains(Codec) ? Codec : "hevc",
            BitrateKbps = BitrateKbps == 0 ? 0 : Math.Clamp(BitrateKbps, 500, 100000),
            Pacing = Pacing,
            CushionMs = Math.Clamp(CushionMs, 0, 50),
            Controller = Controllers.Contains(Controller) ? Controller : "auto",
            HapticsVolume = Math.Clamp(HapticsVolume, 0, 100),
            BackLeft = Buttons.Contains(BackLeft) ? BackLeft : "",
            BackRight = Buttons.Contains(BackRight) ? BackRight : "",
            WakeFirst = WakeFirst,
            RestAfter = RestAfter,
            // An id of an earlier version, or of a look that is gone, follows the desktop.
            Theme = Theming.Themes.Ids.Contains(Theme) ? Theme : Theming.Themes.FollowDesktop,
        };
    }

    /// <summary>The program's arguments for a session with a console, as these settings ask for it.</summary>
    public IReadOnlyList<string> PlayArguments(string console, string address, bool ps4 = false)
    {
        AppSettings s = Valid();
        var arguments = new List<string> { "play", console };
        if (address.Length > 0) arguments.AddRange(["--host", address]);
        if (s.WakeFirst) arguments.Add("--wake");
        arguments.AddRange(["--resolution", s.Resolution.ToString(), "--fps", s.FramesPerSecond.ToString()]);
        // A PS4 streams H.264 only; the program picks that by itself when no codec is named.
        if (!ps4) arguments.AddRange(["--codec", s.Codec]);
        if (s.BitrateKbps > 0) arguments.AddRange(["--bitrate", s.BitrateKbps.ToString()]);
        arguments.AddRange(["--controller", s.Controller]);
        if (!s.Pacing) arguments.AddRange(["--pacing", "off"]);
        else if (s.CushionMs > 0) arguments.AddRange(["--cushion", s.CushionMs.ToString()]);
        arguments.AddRange(["--haptics-volume", s.HapticsVolume.ToString()]);
        if (s.BackLeft.Length > 0) arguments.AddRange(["--back-left", s.BackLeft]);
        if (s.BackRight.Length > 0) arguments.AddRange(["--back-right", s.BackRight]);
        if (s.RestAfter) arguments.Add("--rest");
        return arguments;
    }
}

[JsonSerializable(typeof(AppSettings))]
[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
internal sealed partial class SettingsJson : JsonSerializerContext;

/// <summary>Keeps the settings in the user's configuration folder, beside the program's own files.</summary>
public sealed class SettingsStore(string file)
{
    public string File { get; } = file;

    /// <summary>Where the settings live: $XDG_CONFIG_HOME/ps-vulkanplay/app.json, or under ~/.config.</summary>
    public static string DefaultFile()
    {
        string? config = Environment.GetEnvironmentVariable("XDG_CONFIG_HOME");
        if (string.IsNullOrEmpty(config) || !Path.IsPathRooted(config))
            config = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile, Environment.SpecialFolderOption.DoNotVerify), ".config");
        return Path.Combine(config, "ps-vulkanplay", "app.json");
    }

    /// <summary>A file that is not there, or cannot be read, gives the settings of a first run.</summary>
    public AppSettings Load()
    {
        try
        {
            if (!System.IO.File.Exists(File)) return new AppSettings();
            return (JsonSerializer.Deserialize(System.IO.File.ReadAllText(File), SettingsJson.Default.AppSettings) ?? new AppSettings()).Valid();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return new AppSettings();
        }
    }

    /// <summary>Written beside the file and moved over it. Says what went wrong, or nothing.</summary>
    public string Save(AppSettings settings)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(File)!);
            string beside = File + ".new";
            System.IO.File.WriteAllText(beside, JsonSerializer.Serialize(settings.Valid(), SettingsJson.Default.AppSettings));
            System.IO.File.Move(beside, File, true);
            return "";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"The settings could not be saved to {File}: {ex.Message}";
        }
    }
}
