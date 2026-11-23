using Avalonia.Media;
using PSVulkanPlay.Core;
using PSVulkanPlay.Theming;

namespace PSVulkanPlay.ViewModels;

/// <summary>One choice in a list: what is shown, and the value the program takes.</summary>
public sealed record Choice(string Label, string Value)
{
    public override string ToString() => Label;
}

/// <summary>
/// A look to choose, drawn in its own colors. Colors, not brushes: a model is made on whatever
/// thread asks for it, and a brush belongs to the UI thread.
/// </summary>
public sealed class ThemeChoice(string id, string name, Color left, Color right, Color text, Color accent, Color surface, Command pick) : Observable
{
    private bool _active;

    public string Id { get; } = id;
    public string Name { get; } = name;
    /// <summary>The background's two halves: the same color, but for the choice that follows the desktop.</summary>
    public Color Left { get; } = left;
    public Color Right { get; } = right;
    public Color Text { get; } = text;
    public Color Accent { get; } = accent;
    public Color Surface { get; } = surface;
    public Command Pick { get; } = pick;
    public bool Active { get => _active; set => Set(ref _active, value); }
}

/// <summary>The settings page. Every change is kept at once; there is no Save button to forget.</summary>
public sealed class SettingsViewModel : Observable
{
    private readonly SettingsStore _store;
    private readonly Action<string> _failed;
    private AppSettings _settings;

    public SettingsViewModel(SettingsStore store, Action<string> failed, Action<bool>? showChart = null)
    {
        _store = store;
        _failed = failed;
        _settings = store.Load();
        ThemeChoices = BuildThemeChoices();
        ShowChart = new Command(() => showChart?.Invoke(false));
        ShowHdrChart = new Command(() => showChart?.Invoke(true));
    }

    /// <summary>The test picture in the stream's window, drawn with these settings (SDR, or HDR).</summary>
    public Command ShowChart { get; }
    public Command ShowHdrChart { get; }

    /// <summary>The looks, each as a small picture of itself; the first follows the desktop.</summary>
    public IReadOnlyList<ThemeChoice> ThemeChoices { get; }

    private List<ThemeChoice> BuildThemeChoices()
    {
        var choices = new List<ThemeChoice>();
        void Add(string id, string name, string left, string right, Palette colors) =>
            choices.Add(new ThemeChoice(id, name, Color.Parse(left), Color.Parse(right), Color.Parse(colors.TextPrimary), Color.Parse(colors.Accent), Color.Parse(colors.SurfaceAlt),
                                        new Command(() => Theme = id))
            {
                Active = _settings.Valid().Theme == id,
            });
        // Half dark, half light: whichever the desktop is.
        Add(Themes.FollowDesktop, "Follow the desktop", Themes.Slate.WindowBg, "#4A5160", Themes.Slate);
        foreach (Palette palette in Themes.All) Add(palette.Id, palette.Name, palette.WindowBg, palette.WindowBg, palette);
        return choices;
    }

    /// <summary>Raised when the look changes, with the setting's new value.</summary>
    public event Action<string>? ThemeChanged;

    public AppSettings Current => _settings.Valid();
    public string File => _store.File;

    public IReadOnlyList<Choice> Resolutions { get; } =
        [new("1080p", "1080"), new("720p", "720"), new("540p", "540"), new("360p", "360")];
    public IReadOnlyList<Choice> FrameRates { get; } = [new("60 a second", "60"), new("30 a second", "30")];
    public IReadOnlyList<Choice> Codecs { get; } =
        [new("HEVC (PS5; sharper at the same bitrate)", "hevc"), new("HEVC with HDR (PS5, HDR display)", "hdr"), new("H.264 (PS4, and older graphics cards)", "h264")];
    public IReadOnlyList<Choice> Scalers { get; } =
        [new("EWA Lanczos (the default)", "ewa-lanczos"), new("EWA Lanczos, sharper", "ewa-lanczos-sharp"), new("Lanczos", "lanczos"),
         new("Catmull-Rom", "catmull-rom"), new("Bilinear (soft, the least work)", "bilinear")];
    public IReadOnlyList<Choice> LightBars { get; } =
        [new("As the console sets it", "console"), new("Off", "off"), new("Orange", "ff6a00"), new("Blue", "0050ff"), new("Purple", "8a2be2"),
         new("Green", "00c853"), new("Red", "ff1a1a"), new("White", "ffffff")];
    public IReadOnlyList<Choice> FnActions { get; } =
        [new("Nothing", "none"), new("The menu over the stream", "menu"), new("Statistics", "stats"), new("Full screen", "fullscreen")];
    public IReadOnlyList<Choice> Controllers { get; } =
        [new("Whatever is connected", "auto"), new("A DualSense (haptics, trigger effects)", "dualsense"), new("A DualShock 4", "dualshock4")];
    public IReadOnlyList<Choice> Scales { get; } = AppSettings.Scales.Select(percent => new Choice($"{percent}%", percent.ToString())).ToList();
    public IReadOnlyList<Choice> Buttons { get; } =
        AppSettings.Buttons.Select(name => new Choice(name.Length == 0 ? "Nothing" : ButtonLabel(name), name)).ToList();

    private static string ButtonLabel(string name) => name switch
    {
        "l1" or "r1" or "l3" or "r3" => name.ToUpperInvariant(),
        "ps" => "PS button",
        "share" => "Create",
        "up" or "down" or "left" or "right" => "D-pad " + name,
        _ => char.ToUpperInvariant(name[0]) + name[1..],
    };

    private static Choice Pick(IReadOnlyList<Choice> from, string value) => from.FirstOrDefault(c => c.Value == value) ?? from[0];

    // ---- Button mapping: the controller that is connected, its buttons by its maker's names ----

    /// <summary>What a mapped button can send: as it is, a console button, or nothing.</summary>
    public IReadOnlyList<Choice> MapChoices { get; } =
        new[] { new Choice("As it is", AsItIs) }.Concat(AppSettings.Buttons.Select(name => new Choice(name.Length == 0 ? "Nothing" : ButtonLabel(name), name))).ToList();
    private const string AsItIs = "*";

    private ControllerInfo? _mapped;
    /// <summary>The controller whose buttons are shown for mapping (the app's controller line sets it).</summary>
    public ControllerInfo? MappedController
    {
        get => _mapped;
        set
        {
            if (_mapped?.Name == value?.Name && _mapped?.Kind == value?.Kind) return;
            _mapped = value;
            ButtonRows = value is null ? [] : AppSettings.PadPositions.Select(at => new ButtonRow(this, value.Name, at, PositionLabel(value.Kind, at))).ToList();
            Changed();
            Changed(nameof(ButtonRows));
            Changed(nameof(HasMappedController));
            Changed(nameof(MappingTitle));
        }
    }
    public bool HasMappedController => _mapped is not null;
    public string MappingTitle => _mapped is null ? "Buttons" : $"Buttons of {(_mapped.Name.Length > 0 ? _mapped.Name : "this controller")}";
    public IReadOnlyList<ButtonRow> ButtonRows { get; private set; } = [];

    /// <summary>What a button sends for the controller of that name: AsItIs when not mapped.</summary>
    internal string MappedTo(string pad, string position) =>
        _settings.ButtonMaps.TryGetValue(pad, out var map) && map.TryGetValue(position, out string? to) ? to : AsItIs;

    internal void MapButton(string pad, string position, string to)
    {
        Keep(() =>
        {
            var maps = _settings.ButtonMaps.ToDictionary(one => one.Key, one => new Dictionary<string, string>(one.Value));
            if (!maps.TryGetValue(pad, out var map)) maps[pad] = map = [];
            if (to == AsItIs) map.Remove(position);
            else map[position] = to;
            _settings.ButtonMaps = maps;
        }, nameof(ButtonRows));
    }

    /// <summary>Where a button sits, by the name its maker prints on it.</summary>
    private static string PositionLabel(string kind, string at)
    {
        bool sony = kind is "dualsense" or "dualsense-edge" or "dualshock4", nintendo = kind == "nintendo";
        return at switch
        {
            "south" => sony ? "Cross" : nintendo ? "B" : "A",
            "east" => sony ? "Circle" : nintendo ? "A" : "B",
            "west" => sony ? "Square" : nintendo ? "Y" : "X",
            "north" => sony ? "Triangle" : nintendo ? "X" : "Y",
            "dpad-left" => "D-pad left",
            "dpad-right" => "D-pad right",
            "dpad-up" => "D-pad up",
            "dpad-down" => "D-pad down",
            "left-shoulder" => sony ? "L1" : nintendo ? "L" : "LB",
            "right-shoulder" => sony ? "R1" : nintendo ? "R" : "RB",
            "left-stick" => sony ? "L3" : "Left stick press",
            "right-stick" => sony ? "R3" : "Right stick press",
            "start" => sony ? "Options" : nintendo ? "+" : "Menu",
            "back" => sony ? "Create" : nintendo ? "-" : "View",
            "guide" => sony ? "PS button" : nintendo ? "Home" : "Guide",
            _ => "Touchpad press",
        };
    }

    private void Keep(Action change, string name)
    {
        change();
        _settings = _settings.Valid();
        Changed(name);
        QueueSave(_settings.Valid());
    }

    private readonly object _saveLock = new();
    private AppSettings? _toSave;
    private bool _saving;
    private Task _saver = Task.CompletedTask;

    /// <summary>
    /// Waits until every change made so far is in the file (a moment at most). For the app's
    /// end: a choice made right before closing the window is kept too.
    /// </summary>
    public void Flush()
    {
        for (int i = 0; i < 100; ++i)
        {
            Task saver;
            lock (_saveLock)
            {
                if (!_saving) return;
                saver = _saver;
            }
            saver.Wait(TimeSpan.FromMilliseconds(50));
        }
    }

    // A few hundred bytes to the user's own configuration folder; done off the UI thread all the
    // same, and by one writer at a time: two changes in a row are two saves, and the later one
    // must be the one that stays.
    private void QueueSave(AppSettings settings)
    {
        lock (_saveLock)
        {
            _toSave = settings;
            if (_saving) return;
            _saving = true;
        }
        Task started = Task.Run(() =>
        {
            for (;;)
            {
                AppSettings next;
                lock (_saveLock)
                {
                    if (_toSave is null)
                    {
                        _saving = false;
                        return;
                    }
                    next = _toSave;
                    _toSave = null;
                }
                string error = _store.Save(next);
                if (error.Length > 0) _failed(error);
            }
        });
        lock (_saveLock) _saver = started;
    }

    public Choice Resolution { get => Pick(Resolutions, _settings.Resolution.ToString()); set { if (value is not null) Keep(() => _settings.Resolution = int.Parse(value.Value), nameof(Resolution)); } }
    public Choice FrameRate { get => Pick(FrameRates, _settings.FramesPerSecond.ToString()); set { if (value is not null) Keep(() => _settings.FramesPerSecond = int.Parse(value.Value), nameof(FrameRate)); } }
    public Choice Codec { get => Pick(Codecs, _settings.Codec); set { if (value is not null) Keep(() => _settings.Codec = value.Value, nameof(Codec)); } }
    public Choice Scaler { get => Pick(Scalers, _settings.Scaler); set { if (value is not null) Keep(() => _settings.Scaler = value.Value, nameof(Scaler)); } }
    public bool Deband { get => _settings.Deband; set => Keep(() => _settings.Deband = value, nameof(Deband)); }

    public double Sharpen
    {
        get => _settings.Sharpen;
        set
        {
            Keep(() => _settings.Sharpen = (int)Math.Round(value), nameof(Sharpen));
            Changed(nameof(SharpenText));
        }
    }

    public string SharpenText => _settings.Sharpen == 0 ? "Off: the picture as the console sent it" : $"{_settings.Sharpen}%: more where the picture is soft, none next to black or white";
    public bool HdrOutput { get => _settings.HdrOutput; set => Keep(() => _settings.HdrOutput = value, nameof(HdrOutput)); }
    public Choice Controller { get => Pick(Controllers, _settings.Controller); set { if (value is not null) Keep(() => _settings.Controller = value.Value, nameof(Controller)); } }
    public Choice BackLeft { get => Pick(Buttons, _settings.BackLeft); set { if (value is not null) Keep(() => _settings.BackLeft = value.Value, nameof(BackLeft)); } }
    public bool TriggerEffects { get => _settings.TriggerEffects; set => Keep(() => _settings.TriggerEffects = value, nameof(TriggerEffects)); }
    public Choice LightBar { get => Pick(LightBars, _settings.LightBar); set { if (value is not null) Keep(() => _settings.LightBar = value.Value, nameof(LightBar)); } }
    public bool PlayerLights { get => _settings.PlayerLights; set => Keep(() => _settings.PlayerLights = value, nameof(PlayerLights)); }
    public bool Motion { get => _settings.Motion; set => Keep(() => _settings.Motion = value, nameof(Motion)); }
    public bool Touchpad { get => _settings.Touchpad; set => Keep(() => _settings.Touchpad = value, nameof(Touchpad)); }
    public bool PointerTouch { get => _settings.PointerTouch; set => Keep(() => _settings.PointerTouch = value, nameof(PointerTouch)); }
    public bool DpadTouch { get => _settings.DpadTouch; set => Keep(() => _settings.DpadTouch = value, nameof(DpadTouch)); }
    public Choice FnLeft { get => Pick(FnActions, _settings.FnLeft); set { if (value is not null) Keep(() => _settings.FnLeft = value.Value, nameof(FnLeft)); } }
    public Choice FnRight { get => Pick(FnActions, _settings.FnRight); set { if (value is not null) Keep(() => _settings.FnRight = value.Value, nameof(FnRight)); } }

    public double DeadZone
    {
        get => _settings.DeadZone;
        set
        {
            Keep(() => _settings.DeadZone = (int)Math.Round(value), nameof(DeadZone));
            Changed(nameof(DeadZoneText));
        }
    }

    public string DeadZoneText => _settings.DeadZone == 0 ? "None: every movement counts (the console has its own)" : $"{_settings.DeadZone}% of a stick's throw counts as rest";

    public Choice BackRight { get => Pick(Buttons, _settings.BackRight); set { if (value is not null) Keep(() => _settings.BackRight = value.Value, nameof(BackRight)); } }

    /// <summary>The size of everything in the window, text and controls alike, as a factor of the designed size.</summary>
    public Choice Scale
    {
        get => Pick(Scales, _settings.ScalePercent.ToString());
        set
        {
            if (value is null) return;
            Keep(() => _settings.ScalePercent = int.Parse(value.Value), nameof(Scale));
            Changed(nameof(ScaleFactor));
        }
    }

    public double ScaleFactor => _settings.Valid().ScalePercent / 100.0;

    /// <summary>"system", or a look's id.</summary>
    public string Theme
    {
        get => _settings.Valid().Theme;
        set
        {
            if (value is null || !Themes.Ids.Contains(value)) return;
            Keep(() => _settings.Theme = value, nameof(Theme));
            foreach (ThemeChoice choice in ThemeChoices) choice.Active = choice.Id == _settings.Theme;
            ThemeChanged?.Invoke(_settings.Theme);
        }
    }

    public bool AutoBitrate
    {
        get => _settings.BitrateKbps == 0;
        set
        {
            Keep(() => _settings.BitrateKbps = value ? 0 : 15000, nameof(AutoBitrate));
            Changed(nameof(BitrateMbps));
            Changed(nameof(BitrateText));
        }
    }

    /// <summary>In megabits a second: what people know from their network.</summary>
    public double BitrateMbps
    {
        get => _settings.BitrateKbps == 0 ? 15 : _settings.BitrateKbps / 1000.0;
        set
        {
            Keep(() => _settings.BitrateKbps = (int)Math.Round(value) * 1000, nameof(BitrateMbps));
            Changed(nameof(BitrateText));
            Changed(nameof(AutoBitrate));
        }
    }

    public string BitrateText => _settings.BitrateKbps == 0 ? "Chosen for the picture size" : $"{_settings.BitrateKbps / 1000} Mbit/s";

    // ---- One console's own picture ----

    /// <summary>The registered consoles, by name: the main page says them whenever its list changes.</summary>
    public System.Collections.ObjectModel.ObservableCollection<string> ConsoleNames { get; } = [];
    public bool HasConsoles => ConsoleNames.Count > 0;
    public IReadOnlyList<Choice> Bitrates { get; } =
        [new("Chosen for the picture size", "0"), new("5 Mbit/s", "5000"), new("10 Mbit/s", "10000"), new("15 Mbit/s", "15000"), new("20 Mbit/s", "20000"),
         new("30 Mbit/s", "30000"), new("40 Mbit/s", "40000"), new("50 Mbit/s", "50000")];

    private string _chosenConsole = "";

    public void SetConsoles(IEnumerable<string> names)
    {
        List<string> now = names.Distinct().ToList();
        if (now.SequenceEqual(ConsoleNames)) return;
        ConsoleNames.Clear();
        foreach (string name in now) ConsoleNames.Add(name);
        Changed(nameof(HasConsoles));
        if (!ConsoleNames.Contains(_chosenConsole)) ChosenConsole = ConsoleNames.FirstOrDefault() ?? "";
    }

    public string ChosenConsole
    {
        get => _chosenConsole;
        set
        {
            if (value is null || !Set(ref _chosenConsole, value)) return;
            ConsolePictureChanged();
        }
    }

    private ConsolePicture? Own => _settings.PerConsole.GetValueOrDefault(_chosenConsole);

    /// <summary>The chosen console has a picture of its own; it begins as a copy of the app's.</summary>
    public bool ConsoleOwnPicture
    {
        get => Own is not null;
        set
        {
            if (_chosenConsole.Length == 0 || value == ConsoleOwnPicture) return;
            Keep(() =>
            {
                if (value)
                    _settings.PerConsole[_chosenConsole] = new ConsolePicture
                        { Resolution = _settings.Resolution, FramesPerSecond = _settings.FramesPerSecond, Codec = _settings.Codec, BitrateKbps = _settings.BitrateKbps };
                else _settings.PerConsole.Remove(_chosenConsole);
            }, nameof(ConsoleOwnPicture));
            ConsolePictureChanged();
        }
    }

    public Choice ConsoleResolution
    {
        get => Pick(Resolutions, (Own?.Resolution ?? _settings.Resolution).ToString());
        set { if (value is not null && Own is not null) Keep(() => Own!.Resolution = int.Parse(value.Value), nameof(ConsoleResolution)); }
    }
    public Choice ConsoleFrameRate
    {
        get => Pick(FrameRates, (Own?.FramesPerSecond ?? _settings.FramesPerSecond).ToString());
        set { if (value is not null && Own is not null) Keep(() => Own!.FramesPerSecond = int.Parse(value.Value), nameof(ConsoleFrameRate)); }
    }
    public Choice ConsoleCodec
    {
        get => Pick(Codecs, Own?.Codec ?? _settings.Codec);
        set { if (value is not null && Own is not null) Keep(() => Own!.Codec = value.Value, nameof(ConsoleCodec)); }
    }
    public Choice ConsoleBitrate
    {
        get => Pick(Bitrates, (Own?.BitrateKbps ?? _settings.BitrateKbps).ToString());
        set { if (value is not null && Own is not null) Keep(() => Own!.BitrateKbps = int.Parse(value.Value), nameof(ConsoleBitrate)); }
    }

    private void ConsolePictureChanged()
    {
        foreach (string name in new[] { nameof(ConsoleOwnPicture), nameof(ConsoleResolution), nameof(ConsoleFrameRate), nameof(ConsoleCodec), nameof(ConsoleBitrate) }) Changed(name);
    }

    public bool GpuDecoding { get => _settings.GpuDecoding; set => Keep(() => _settings.GpuDecoding = value, nameof(GpuDecoding)); }
    public bool Reconnect { get => _settings.Reconnect; set => Keep(() => _settings.Reconnect = value, nameof(Reconnect)); }

    public bool Pacing
    {
        get => _settings.Pacing;
        set
        {
            Keep(() => _settings.Pacing = value, nameof(Pacing));
            Changed(nameof(CushionText));
        }
    }

    public double Cushion
    {
        get => _settings.CushionMs;
        set
        {
            Keep(() => _settings.CushionMs = (int)Math.Round(value), nameof(Cushion));
            Changed(nameof(CushionText));
        }
    }

    public double LossCeiling
    {
        get => _settings.LossCeiling;
        set
        {
            Keep(() => _settings.LossCeiling = (int)Math.Round(value), nameof(LossCeiling));
            Changed(nameof(LossCeilingText));
        }
    }

    public string LossCeilingText => _settings.LossCeiling switch
    {
        100 => "All of it",
        0 => "None",
        AppSettings.DefaultLossCeiling => $"Up to {AppSettings.DefaultLossCeiling}% (the default)",
        int percent => $"Up to {percent}%",
    };

    /// <summary>In steps of 100 cd/m² on the slider (400 to 4000); the file may hold any value the program takes.</summary>
    public double HdrPeak
    {
        get => _settings.HdrPeakNits;
        set
        {
            Keep(() => _settings.HdrPeakNits = (int)Math.Round(value), nameof(HdrPeak));
            Changed(nameof(HdrPeakText));
        }
    }

    public string HdrPeakText => HdrPeakMeasured ? "Measured from each picture (the default)" : $"{_settings.HdrPeakNits} cd/m²";

    /// <summary>Each HDR picture measured, rather than the brightness on the slider.</summary>
    public bool HdrPeakMeasured
    {
        get => _settings.HdrPeakMeasured ?? _settings.HdrPeakNits == AppSettings.DefaultHdrPeakNits;
        set
        {
            Keep(() => _settings.HdrPeakMeasured = value, nameof(HdrPeakMeasured));
            Changed(nameof(HdrPeakText));
        }
    }

    public string CushionText => !_settings.Pacing ? "Off with even pacing" : _settings.CushionMs == 0 ? "None: the least delay" : $"{_settings.CushionMs} ms of added delay";

    public double HapticsVolume
    {
        get => _settings.HapticsVolume;
        set
        {
            Keep(() => _settings.HapticsVolume = (int)Math.Round(value), nameof(HapticsVolume));
            Changed(nameof(HapticsText));
        }
    }

    public string HapticsText => _settings.HapticsVolume == 0 ? "Off" : $"{_settings.HapticsVolume}%";

    public double LightBrightness
    {
        get => _settings.LightBrightness;
        set
        {
            Keep(() => _settings.LightBrightness = (int)Math.Round(value), nameof(LightBrightness));
            Changed(nameof(LightBrightnessText));
        }
    }

    public string LightBrightnessText => _settings.LightBrightness == 0 ? "Off" : $"{_settings.LightBrightness}%";

    public double RumbleStrength
    {
        get => _settings.RumbleStrength;
        set
        {
            Keep(() => _settings.RumbleStrength = (int)Math.Round(value), nameof(RumbleStrength));
            Changed(nameof(RumbleText));
        }
    }

    public string RumbleText => _settings.RumbleStrength == 0 ? "Off: the controller never rumbles" : $"{_settings.RumbleStrength}%; also the haptics of a DualSense away from its cable";

    public bool WakeFirst { get => _settings.WakeFirst; set => Keep(() => _settings.WakeFirst = value, nameof(WakeFirst)); }
    public bool RestAfter { get => _settings.RestAfter; set => Keep(() => _settings.RestAfter = value, nameof(RestAfter)); }
    public bool ShowStats { get => _settings.ShowStats; set => Keep(() => _settings.ShowStats = value, nameof(ShowStats)); }
}

/// <summary>One button of the controller being mapped: its place, its maker's name, and what it sends.</summary>
public sealed class ButtonRow : Observable
{
    private readonly SettingsViewModel _owner;
    private readonly string _pad;

    public ButtonRow(SettingsViewModel owner, string pad, string position, string label)
    {
        _owner = owner;
        _pad = pad;
        Position = position;
        Label = label;
        // The first choice says what the button sends when it is left as it is.
        Choices = [new Choice($"As it is: {owner.MapChoices.First(c => c.Value == DefaultFor(position)).Label}", owner.MapChoices[0].Value), .. owner.MapChoices.Skip(1)];
    }

    public string Label { get; }
    public string Position { get; }
    public IReadOnlyList<Choice> Choices { get; }
    public Choice Selected
    {
        get => Choices.FirstOrDefault(c => c.Value == _owner.MappedTo(_pad, Position)) ?? Choices[0];
        set
        {
            if (value is null) return;
            _owner.MapButton(_pad, Position, value.Value);
            Changed();
        }
    }

    /// <summary>The console button a button at that place sends unless mapped.</summary>
    private static string DefaultFor(string position) => position switch
    {
        "south" => "cross", "east" => "circle", "west" => "square", "north" => "triangle",
        "dpad-left" => "left", "dpad-right" => "right", "dpad-up" => "up", "dpad-down" => "down",
        "left-shoulder" => "l1", "right-shoulder" => "r1", "left-stick" => "l3", "right-stick" => "r3",
        "start" => "options", "back" => "share", "guide" => "ps", _ => "touchpad",
    };
}
