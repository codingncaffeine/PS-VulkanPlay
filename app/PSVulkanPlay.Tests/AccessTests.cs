using Avalonia.Media;
using PSVulkanPlay.Core;
using PSVulkanPlay.Theming;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>What makes the app readable: every look's text against its backgrounds, and the size setting.</summary>
public sealed class AccessTests : IDisposable
{
    private readonly string _folder = Directory.CreateTempSubdirectory("psvp-access-").FullName;

    public void Dispose() => Directory.Delete(_folder, true);

    // WCAG 2.2's relative luminance and contrast ratio.
    private static double Luminance(string hex)
    {
        Color c = Color.Parse(hex);
        static double Channel(byte value)
        {
            double v = value / 255.0;
            return v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4);
        }
        return 0.2126 * Channel(c.R) + 0.7152 * Channel(c.G) + 0.0722 * Channel(c.B);
    }

    public static double Contrast(string a, string b)
    {
        double la = Luminance(a), lb = Luminance(b);
        return (Math.Max(la, lb) + 0.05) / (Math.Min(la, lb) + 0.05);
    }

    public static TheoryData<string> Looks() => [.. Themes.All.Select(palette => palette.Id)];

    [Theory]
    [MemberData(nameof(Looks))]
    public void Every_looks_text_reads_on_every_background_it_is_put_on(string id)
    {
        Palette p = Themes.All.Single(palette => palette.Id == id);
        // Text: WCAG AA for body text (4.5:1) on the window, the cards and the raised parts of cards.
        foreach (string background in new[] { p.WindowBg, p.Surface, p.SurfaceAlt })
        {
            Assert.True(Contrast(p.TextPrimary, background) >= 4.5, $"{p.Name}: text {p.TextPrimary} on {background}: {Contrast(p.TextPrimary, background):F2}");
            Assert.True(Contrast(p.TextSecondary, background) >= 4.5, $"{p.Name}: secondary text {p.TextSecondary} on {background}: {Contrast(p.TextSecondary, background):F2}");
        }
        // The words on an accent button, and the accent's own shapes (3:1 for parts of the interface).
        Assert.True(Contrast(p.OnAccent, p.Accent) >= 4.5, $"{p.Name}: on-accent {Contrast(p.OnAccent, p.Accent):F2}");
        Assert.True(Contrast(p.OnAccent, p.AccentHover) >= 4.5, $"{p.Name}: on-accent under the pointer {Contrast(p.OnAccent, p.AccentHover):F2}");
        Assert.True(Contrast(p.Accent, p.Surface) >= 3.0, $"{p.Name}: accent on a card {Contrast(p.Accent, p.Surface):F2}");
    }

    [Fact]
    public void High_contrast_goes_past_AAA_and_draws_its_lines_in_full()
    {
        Palette p = Themes.Contrast;
        Assert.True(Contrast(p.TextPrimary, p.WindowBg) >= 20);
        Assert.True(Contrast(p.TextSecondary, p.WindowBg) >= 7, "AAA for the secondary text too");
        Assert.True(Contrast(p.Line, p.Surface) >= 7, "the cards' outlines are seen, not guessed");
        Assert.True(Contrast(p.Accent, p.WindowBg) >= 7);
        foreach (string state in new[] { p.Good, p.Resting, p.Bad }) Assert.True(Contrast(state, p.Surface) >= 4.5, state);
    }

    [Theory]
    [InlineData(100, 100)]
    [InlineData(150, 150)]
    [InlineData(200, 200)]
    [InlineData(130, 100)]
    [InlineData(0, 100)]
    public void The_size_is_one_the_app_offers(int asked, int kept) =>
        Assert.Equal(kept, new AppSettings { ScalePercent = asked }.Valid().ScalePercent);

    [Fact]
    public void A_size_and_the_high_contrast_look_are_kept()
    {
        var store = new SettingsStore(Path.Combine(_folder, "app.json"));
        Assert.Equal("", store.Save(new AppSettings { ScalePercent = 175, Theme = "contrast" }));
        Assert.Equal(175, store.Load().ScalePercent);
        Assert.Equal("contrast", store.Load().Theme);
    }
}
