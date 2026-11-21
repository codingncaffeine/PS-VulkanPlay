using Avalonia;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;

namespace PSVulkanPlay.Theming;

/// <summary>One look of the app: a name and its colors.</summary>
public sealed record Palette(string Id, string Name, bool Dark,
                             string WindowBg, string Surface, string SurfaceAlt, string Line,
                             string Accent, string AccentHover, string OnAccent,
                             string TextPrimary, string TextSecondary,
                             string Good, string Resting, string Bad);

/// <summary>
/// The looks to choose from. A look is applied by putting its colors into the app's resources
/// as brushes; every view takes its colors from there (DynamicResource), so a change shows at once.
/// </summary>
public static class Themes
{
    /// <summary>The setting that follows the desktop: the slate look when the desktop is dark, the light one when it is not.</summary>
    public const string FollowDesktop = "system";

    // Every look keeps WCAG AA: text 4.5:1 on its backgrounds, the words on an accent button 4.5:1 also
    // under the pointer, the accent 3:1 on a card (tests/AccessTests).
    public static readonly Palette Slate = new("slate", "Slate", true,
        "#14171C", "#1D2128", "#272C35", "#343A45", "#F0702D", "#FF8A47", "#16110D", "#ECEEF1", "#9AA1AD", "#4CC38A", "#E6A23C", "#E5605A");

    public static readonly Palette Light = new("light", "Light", false,
        "#F2F3F5", "#FFFFFF", "#E9EBEF", "#D3D7DE", "#C4501A", "#B04716", "#FFFFFF", "#1B1E23", "#5F6672", "#1F9D62", "#B7791F", "#C93D36");

    public static readonly Palette Oled = new("oled", "OLED Black", true,
        "#000000", "#0B0B0C", "#161618", "#2A2A2D", "#F0702D", "#FF8A47", "#16110D", "#E8E8EA", "#85858C", "#4CC38A", "#E6A23C", "#E5605A");

    public static readonly Palette Midnight = new("midnight", "Midnight Blue", true,
        "#0A0E1A", "#111827", "#1E293B", "#334155", "#2B6CE6", "#2361D6", "#FFFFFF", "#F1F5F9", "#94A3B8", "#34D399", "#FBBF24", "#F87171");

    public static readonly Palette Ember = new("ember", "Ember", true,
        "#17110F", "#211815", "#2D211C", "#43322A", "#FF7A3D", "#FF9864", "#1A0E08", "#F4EDE8", "#B09C90", "#5CCB8F", "#EDB04A", "#EE6A5F");

    // For low vision: white on black, a yellow accent, every line drawn in full white (text 21:1, secondary 15:1).
    public static readonly Palette Contrast = new("contrast", "High Contrast", true,
        "#000000", "#000000", "#1F1F1F", "#FFFFFF", "#FFE500", "#FFF27A", "#000000", "#FFFFFF", "#DADADA", "#5CFF9D", "#FFE500", "#FF7B72");

    public static readonly IReadOnlyList<Palette> All = [Slate, Light, Oled, Midnight, Ember, Contrast];

    public static IReadOnlyList<string> Ids { get; } = [FollowDesktop, .. All.Select(palette => palette.Id)];

    /// <summary>The look a setting stands for; one that is not known is the desktop's.</summary>
    public static Palette Resolve(string id, bool desktopIsDark) =>
        All.FirstOrDefault(palette => palette.Id == id) ?? (desktopIsDark ? Slate : Light);

    public static bool DesktopIsDark(Application app) =>
        app.PlatformSettings?.GetColorValues().ThemeVariant == PlatformThemeVariant.Dark;

    public static void Apply(Application app, Palette palette)
    {
        ThemeVariant variant = palette.Dark ? ThemeVariant.Dark : ThemeVariant.Light;
        Color accent = Color.Parse(palette.Accent);
        // The theme's own controls (sliders, switches, check boxes, the focus ring) take their
        // color from its palette of that variant.
        if (app.Styles.OfType<FluentTheme>().FirstOrDefault() is { } fluent)
        {
            if (fluent.Palettes.TryGetValue(variant, out ColorPaletteResources? own)) own.Accent = accent;
            else fluent.Palettes[variant] = new ColorPaletteResources { Accent = accent };
        }
        app.RequestedThemeVariant = variant;
        void Set(string key, string hex) => app.Resources[key] = new SolidColorBrush(Color.Parse(hex));
        Set("WindowBg", palette.WindowBg);
        Set("Surface", palette.Surface);
        Set("SurfaceAlt", palette.SurfaceAlt);
        Set("Line", palette.Line);
        Set("Accent", palette.Accent);
        Set("AccentHover", palette.AccentHover);
        Set("OnAccent", palette.OnAccent);
        Set("TextPrimary", palette.TextPrimary);
        Set("TextSecondary", palette.TextSecondary);
        Set("Good", palette.Good);
        Set("Resting", palette.Resting);
        Set("Bad", palette.Bad);
        // A switch that is on: its knob in the color of the words on an accent button. The theme's own
        // white is lost on a light accent (1.2:1 on High Contrast's yellow, 2.9:1 on Slate's orange).
        foreach (string state in new[] { "", "PointerOver", "Pressed" }) Set("ToggleSwitchKnobFillOn" + state, palette.OnAccent);
    }
}
