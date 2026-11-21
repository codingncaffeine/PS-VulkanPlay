#include <algorithm>
#include <string>
#include <vector>

#include "check.h"
#include "overlay/text.h"

using namespace psvp::overlay;

// The atlas and its table belong together: every glyph lies inside the atlas, in the order of the
// characters, and the distance field reads "inside" in the middle of a solid glyph and "far outside" in a corner.
PSVP_TEST(overlay_atlas_and_table_agree) {
    CHECK(std::size(kAtlasGlyphs) > 180);
    CHECK(std::is_sorted(std::begin(kAtlasGlyphs), std::end(kAtlasGlyphs), [](const AtlasGlyph& a, const AtlasGlyph& b) { return a.codepoint < b.codepoint; }));
    for (const AtlasGlyph& g : kAtlasGlyphs) {
        CHECK(g.x >= 0 && g.y >= 0 && g.x + g.width <= kAtlasWidth && g.y + g.height <= kAtlasHeight);
        CHECK(g.advance > 0);
    }
    const AtlasGlyph& l = glyph_of(U'l');  // a solid upright stroke
    CHECK(l.width > 0 && l.height > 0);
    const std::uint8_t middle = atlas_pixels()[(l.y + l.height / 2) * kAtlasWidth + l.x + l.width / 2];
    CHECK(middle > 128);
    CHECK(atlas_pixels()[0] < 128);
    CHECK(glyph_of(U' ').width == 0 && glyph_of(U' ').advance > 0);
}

PSVP_TEST(overlay_reads_utf8_and_stands_in_for_what_it_has_not) {
    CHECK(decode_utf8("A\xc3\x97" "b\xe2\x80\xa6") == U"A×b…");
    CHECK(decode_utf8("\xff" "x") == U"�x");
    CHECK(decode_utf8("\xe2\x80") == U"��");
    // A character the atlas lacks is drawn as '?'.
    CHECK(&glyph_of(U'一') == &glyph_of(U'?'));
    CHECK(glyph_of(U'×').codepoint == U'×');
}

PSVP_TEST(overlay_text_scales_with_its_size) {
    const float at_16 = text_width("Statistics", 16);
    const float at_32 = text_width("Statistics", 32);
    CHECK(at_16 > 40 && at_16 < 120);
    CHECK(at_32 > at_16 * 1.99F && at_32 < at_16 * 2.01F);
    CHECK(text_width("", 20) == 0.0F);
    CHECK(line_height(20) > 20 && line_height(20) < 30);
    // One quad per visible glyph: none for the spaces.
    std::vector<Quad> quads;
    const float end = append_text(quads, "a b", 10, 50, 20, kStatsTextColor);
    CHECK(quads.size() == 2);
    CHECK(end > 10 + text_width("a b", 20) - 0.01F && end < 10 + text_width("a b", 20) + 0.01F);
    for (const Quad& q : quads) {
        CHECK(q.glyph && q.color == kStatsTextColor);
        CHECK(q.x1 > q.x0 && q.y1 > q.y0 && q.u1 > q.u0 && q.v1 > q.v0);
        CHECK(q.y0 < 50 && q.y1 > 50 - 20 * 0.4F);  // standing on the baseline, its margin reaching below it
    }
}

PSVP_TEST(overlay_stats_panel_sits_in_the_corner_and_fits_its_lines) {
    const std::vector<std::string> lines{"Statistics (F3 hides them)", "1920×1080 HEVC · 60 pictures a second", "Frames lost on the way: 3"};
    const std::vector<Quad> quads = stats_panel(lines, 1280, 720);
    CHECK(!quads.empty() && !quads.front().glyph && quads.front().color == kPanelColor);
    const Quad& panel = quads.front();
    CHECK(panel.x0 > 0 && panel.y0 > 0 && panel.x1 < 1280 && panel.y1 < 720);
    for (std::size_t i = 1; i < quads.size(); ++i) {
        CHECK(quads[i].glyph);
        CHECK(quads[i].x0 >= panel.x0 && quads[i].x1 <= panel.x1 + 1 && quads[i].y0 >= panel.y0 - 1 && quads[i].y1 <= panel.y1 + 1);
    }
    // The title in the accent color, the rest in the text color.
    CHECK(quads[1].color == kStatsTitleColor && quads.back().color == kStatsTextColor);
    // A larger window, larger text; nothing to say, nothing drawn.
    CHECK(stats_panel(lines, 2560, 1440).front().x1 > panel.x1 * 1.5F);
    CHECK(stats_panel({}, 1280, 720).empty());
}
