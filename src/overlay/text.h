// Text and panels drawn over the stream, laid out as quads in window pixels: no
// graphics calls here. The glyphs come from the atlas made by tools/glyph-atlas
// (Inter Medium as signed distance fields), so any text size is drawn from one
// small image.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psvp::overlay {

struct AtlasGlyph {
    char32_t codepoint;
    int x, y, width, height;  // in the atlas, the distance margin included
    int left, top;            // from the pen position to the bitmap's top-left corner, y up
    int advance;              // in 1/64 pixel, at the atlas's size
};

#include "overlay/inter_glyphs.inc"

// The atlas's pixels: kAtlasWidth x kAtlasHeight bytes, 128 = on the outline, more = inside.
[[nodiscard]] const std::uint8_t* atlas_pixels() noexcept;

// sRGB bytes and an opacity.
struct Color {
    std::uint8_t r = 255, g = 255, b = 255;
    float alpha = 1.0F;
    friend bool operator==(const Color&, const Color&) = default;
};

// One rectangle: a solid one (a panel), or a glyph cut out of the atlas.
struct Quad {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // window pixels, y down
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // in the atlas, 0 to 1
    Color color;
    bool glyph = false;
};

// The glyph of a character, or of '?' for one the atlas does not have.
[[nodiscard]] const AtlasGlyph& glyph_of(char32_t codepoint) noexcept;
// The characters of UTF-8 text; a byte that does not begin a valid sequence counts as U+FFFD.
[[nodiscard]] std::u32string decode_utf8(std::string_view text);

// Sizes are the text's pixel size, as a font size in pixels.
[[nodiscard]] float text_width(std::string_view text, float size);
[[nodiscard]] float line_height(float size) noexcept;
[[nodiscard]] float ascender(float size) noexcept;

// Appends the glyphs of one line: the pen starts at x, on the baseline. Returns where the pen ended.
float append_text(std::vector<Quad>& quads, std::string_view text, float x, float baseline, float size, Color color);
void append_panel(std::vector<Quad>& quads, float x0, float y0, float x1, float y1, Color color);

// The statistics panel in the top-left corner of a window of the given size: its lines in a
// dark panel, sized to the window (text about a 45th of its height, 13 px at least).
[[nodiscard]] std::vector<Quad> stats_panel(const std::vector<std::string>& lines, float window_width, float window_height);

// The panel's colors, as they reach the screen where the text is solid.
inline constexpr Color kPanelColor{20, 23, 28, 0.85F};
inline constexpr Color kStatsTextColor{236, 239, 244, 1.0F};
inline constexpr Color kStatsTitleColor{240, 112, 45, 1.0F};

}  // namespace psvp::overlay
