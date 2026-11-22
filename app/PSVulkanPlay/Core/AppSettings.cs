using System.Text.Json;
using System.Text.Json.Serialization;

namespace PSVulkanPlay.Core;

/// <summary>One console's own picture: used for it in place of the app's when it is there.</summary>
public sealed class ConsolePicture
{
    public int Resolution { get; set; } = 1080;
    public int FramesPerSecond { get; set; } = 60;
    public string Codec { get; set; } = "hevc";
    public int BitrateKbps { get; set; }                // 0: the program's own choice

    public ConsolePicture Valid() => new()
    {
        Resolution = AppSettings.Resolutions.Contains(Resolution) ? Resolution : 1080,
        FramesPerSecond = AppSettings.FrameRates.Contains(FramesPerSecond) ? FramesPerSecond : 60,
        Codec = AppSettings.Codecs.Contains(Codec) ? Codec : "hevc",
        BitrateKbps = BitrateKbps == 0 ? 0 : Math.Clamp(BitrateKbps, 500, 100000),
    };
}

/// <summary>What the user has chosen. Every value is one the stream program takes as an option.</summary>
public sealed class AppSettings
{
    public int Resolution { get; set; } = 1080;         // 360, 540, 720, 1080
    public int FramesPerSecond { get; set; } = 60;      // 30, 60
    public string Codec { get; set; } = "hevc";         // h264, hevc, hdr
    public int BitrateKbps { get; set; }                // 0: the program's own choice
    public bool GpuDecoding { get; set; } = true;       // on the graphics card where it can (checked against software first)
    public bool Reconnect { get; set; } = true;         // a connection lost while playing is made again (for a minute)
    public bool Pacing { get; set; } = true;
    public int CushionMs { get; set; }                  // 0 to 50
    public int LossCeiling { get; set; } = DefaultLossCeiling;   // percent, 0 to 100: the most loss the console is told of
    public int HdrPeakNits { get; set; } = DefaultHdrPeakNits;   // how bright an HDR picture is taken to be at its brightest
    // Each HDR picture measured instead. Not in a file of an earlier version: measured, unless that file set a brightness.
    public bool? HdrPeakMeasured { get; set; }
    public bool HdrOutput { get; set; } = true;         // an HDR picture goes to the screen as HDR where the desktop takes it
    public string Scaler { get; set; } = DefaultScaler;   // how the picture is brought to the window's size (Scalers)
    public bool Deband { get; set; } = true;            // banding smoothed out of the picture
    public string Controller { get; set; } = "auto";    // auto, dualsense, dualshock4
    public int HapticsVolume { get; set; } = 100;       // 0 to 100
    public int RumbleStrength { get; set; } = 100;      // 0 to 100: the console's rumble and the rumble made from haptics
    public string BackLeft { get; set; } = "";          // a button's name, or nothing
    public string BackRight { get; set; } = "";
    public bool TriggerEffects { get; set; } = true;    // the console's adaptive trigger effects reach a DualSense
    public string LightBar { get; set; } = "console";   // "console", "off", or a color as six hexadecimal digits
    public bool PlayerLights { get; set; } = true;
    public bool Motion { get; set; } = true;            // the motion sensors are told to the console
    public bool Touchpad { get; set; } = true;          // touches are told to the console
    public bool PointerTouch { get; set; } = true;      // the mouse and a touch screen on the stream's window stand in for the touchpad
    public bool DpadTouch { get; set; } = true;         // on a controller with no touchpad: share held + the d-pad stand in for it
    public int DeadZone { get; set; }                   // percent of a stick's throw that counts as rest, 0 to 30
    public string FnLeft { get; set; } = "none";        // a DualSense Edge's Fn buttons: none, menu, stats, fullscreen
    public string FnRight { get; set; } = "none";
    public bool WakeFirst { get; set; } = true;
    public bool RestAfter { get; set; }
    public bool ShowStats { get; set; }                 // the statistics over the picture from the start (F3 at any time)
    public string Theme { get; set; } = "system";       // "system", or a look's id (Theming/Themes.cs)
    public int ScalePercent { get; set; } = 100;        // the size of everything in the app's window: 100, 125, 150, 175, 200
    /// <summary>The consoles that have a picture of their own, by name.</summary>
    public Dictionary<string, ConsolePicture> PerConsole { get; set; } = [];

    public const int DefaultLossCeiling = 5;
    public const int DefaultHdrPeakNits = 1000, LeastHdrPeakNits = 400, MostHdrPeakNits = 10000;
    public static readonly int[] Resolutions = [360, 540, 720, 1080];
    public static readonly int[] FrameRates = [30, 60];
    public static readonly string[] Codecs = ["h264", "hevc", "hdr"];
    public const string DefaultScaler = "ewa-lanczos";
    public static readonly string[] Scalers = ["bilinear", "catmull-rom", "lanczos", "ewa-lanczos", "ewa-lanczos-sharp"];
    public static readonly string[] Controllers = ["auto", "dualsense", "dualshock4"];
    public static readonly string[] FnActions = ["none", "menu", "stats", "fullscreen"];
    public const int MostDeadZone = 30;

    /// <summary>"console", "off", or six hexadecimal digits, as the program takes them.</summary>
    public static bool IsLightBar(string text) =>
        text is "console" or "off" || (text.Length == 6 && text.All(Uri.IsHexDigit));
    public static readonly int[] Scales = [100, 125, 150, 175, 200];

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
            GpuDecoding = GpuDecoding,
            Reconnect = Reconnect,
            Pacing = Pacing,
            CushionMs = Math.Clamp(CushionMs, 0, 50),
            LossCeiling = Math.Clamp(LossCeiling, 0, 100),
            HdrPeakNits = Math.Clamp(HdrPeakNits, LeastHdrPeakNits, MostHdrPeakNits),
            HdrPeakMeasured = HdrPeakMeasured ?? HdrPeakNits == DefaultHdrPeakNits,
            HdrOutput = HdrOutput,
            Scaler = Scalers.Contains(Scaler) ? Scaler : DefaultScaler,
            Deband = Deband,
            Controller = Controllers.Contains(Controller) ? Controller : "auto",
            HapticsVolume = Math.Clamp(HapticsVolume, 0, 100),
            RumbleStrength = Math.Clamp(RumbleStrength, 0, 100),
            BackLeft = Buttons.Contains(BackLeft) ? BackLeft : "",
            BackRight = Buttons.Contains(BackRight) ? BackRight : "",
            TriggerEffects = TriggerEffects,
            LightBar = IsLightBar(LightBar ?? "") ? LightBar!.ToLowerInvariant() : "console",
            PlayerLights = PlayerLights,
            Motion = Motion,
            Touchpad = Touchpad,
            PointerTouch = PointerTouch,
            DpadTouch = DpadTouch,
            DeadZone = Math.Clamp(DeadZone, 0, MostDeadZone),
            FnLeft = FnActions.Contains(FnLeft) ? FnLeft : "none",
            FnRight = FnActions.Contains(FnRight) ? FnRight : "none",
            WakeFirst = WakeFirst,
            RestAfter = RestAfter,
            ShowStats = ShowStats,
            // An id of an earlier version, or of a look that is gone, follows the desktop.
            Theme = Theming.Themes.Ids.Contains(Theme) ? Theme : Theming.Themes.FollowDesktop,
            ScalePercent = Scales.Contains(ScalePercent) ? ScalePercent : 100,
            PerConsole = (PerConsole ?? []).Where(entry => entry.Key.Length > 0 && entry.Value is not null)
                                           .ToDictionary(entry => entry.Key, entry => entry.Value.Valid()),
        };
    }

    /// <summary>The program's arguments for a session with a console, as these settings ask for it.</summary>
    public IReadOnlyList<string> PlayArguments(string console, string address, bool ps4 = false)
    {
        AppSettings s = Valid();
        var arguments = new List<string> { "play", console };
        if (address.Length > 0) arguments.AddRange(["--host", address]);
        if (s.WakeFirst) arguments.Add("--wake");
        // The console's own picture where it has one, the app's otherwise.
        ConsolePicture picture = s.PerConsole.TryGetValue(console, out ConsolePicture? own)
            ? own
            : new ConsolePicture { Resolution = s.Resolution, FramesPerSecond = s.FramesPerSecond, Codec = s.Codec, BitrateKbps = s.BitrateKbps };
        arguments.AddRange(["--resolution", picture.Resolution.ToString(), "--fps", picture.FramesPerSecond.ToString()]);
        // A PS4 streams H.264 only; the program picks that by itself when no codec is named.
        if (!ps4) arguments.AddRange(["--codec", picture.Codec]);
        if (picture.BitrateKbps > 0) arguments.AddRange(["--bitrate", picture.BitrateKbps.ToString()]);
        if (!s.GpuDecoding) arguments.AddRange(["--decoder", "software"]);
        if (!s.Reconnect) arguments.AddRange(["--reconnect", "0"]);
        arguments.AddRange(["--controller", s.Controller]);
        if (!s.Pacing) arguments.AddRange(["--pacing", "off"]);
        else if (s.CushionMs > 0) arguments.AddRange(["--cushion", s.CushionMs.ToString()]);
        if (s.LossCeiling != DefaultLossCeiling) arguments.AddRange(["--loss-ceiling", s.LossCeiling.ToString()]);
        if (s.HdrPeakMeasured == false) arguments.AddRange(["--hdr-peak", s.HdrPeakNits.ToString()]);
        if (!s.HdrOutput) arguments.AddRange(["--hdr-output", "off"]);
        if (s.Scaler != DefaultScaler) arguments.AddRange(["--scaler", s.Scaler]);
        if (!s.Deband) arguments.AddRange(["--deband", "off"]);
        arguments.AddRange(["--haptics-volume", s.HapticsVolume.ToString()]);
        if (s.BackLeft.Length > 0) arguments.AddRange(["--back-left", s.BackLeft]);
        if (s.BackRight.Length > 0) arguments.AddRange(["--back-right", s.BackRight]);
        if (!s.TriggerEffects) arguments.AddRange(["--trigger-effects", "off"]);
        if (s.LightBar != "console") arguments.AddRange(["--light-bar", s.LightBar]);
        if (!s.PlayerLights) arguments.AddRange(["--player-lights", "off"]);
        if (!s.Motion) arguments.AddRange(["--motion", "off"]);
        if (!s.Touchpad) arguments.AddRange(["--touchpad", "off"]);
        if (s.DeadZone != 0) arguments.AddRange(["--dead-zone", s.DeadZone.ToString()]);
        if (s.RumbleStrength != 100) arguments.AddRange(["--rumble-strength", s.RumbleStrength.ToString()]);
        if (!s.PointerTouch) arguments.AddRange(["--pointer-touch", "off"]);
        if (!s.DpadTouch) arguments.AddRange(["--dpad-touch", "off"]);
        if (s.FnLeft != "none") arguments.AddRange(["--fn-left", s.FnLeft]);
        if (s.FnRight != "none") arguments.AddRange(["--fn-right", s.FnRight]);
        if (s.ShowStats) arguments.Add("--stats");
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
