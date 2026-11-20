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

    public SettingsViewModel(SettingsStore store, Action<string> failed)
    {
        _store = store;
        _failed = failed;
        _settings = store.Load();
        ThemeChoices = BuildThemeChoices();
    }

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
    public IReadOnlyList<Choice> Controllers { get; } =
        [new("Whatever is connected", "auto"), new("A DualSense (haptics, trigger effects)", "dualsense"), new("A DualShock 4", "dualshock4")];
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
    public Choice Controller { get => Pick(Controllers, _settings.Controller); set { if (value is not null) Keep(() => _settings.Controller = value.Value, nameof(Controller)); } }
    public Choice BackLeft { get => Pick(Buttons, _settings.BackLeft); set { if (value is not null) Keep(() => _settings.BackLeft = value.Value, nameof(BackLeft)); } }
    public Choice BackRight { get => Pick(Buttons, _settings.BackRight); set { if (value is not null) Keep(() => _settings.BackRight = value.Value, nameof(BackRight)); } }

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

    public bool WakeFirst { get => _settings.WakeFirst; set => Keep(() => _settings.WakeFirst = value, nameof(WakeFirst)); }
    public bool RestAfter { get => _settings.RestAfter; set => Keep(() => _settings.RestAfter = value, nameof(RestAfter)); }
}
