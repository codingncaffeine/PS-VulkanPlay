#include "overlay/text.h"

#include <algorithm>
#include <cmath>

namespace psvp::overlay {
namespace {

constexpr std::uint8_t kAtlas[] = {
#include "psvp/overlay_atlas.inc"
};
static_assert(sizeof(kAtlas) == static_cast<std::size_t>(kAtlasWidth) * static_cast<std::size_t>(kAtlasHeight),
              "the atlas image and its glyph table were made together: make them again with tools/glyph-atlas/make.sh");

}  // namespace

const std::uint8_t* atlas_pixels() noexcept { return kAtlas; }

const AtlasGlyph& glyph_of(char32_t codepoint) noexcept {
    // The table is in the order of the characters.
    const auto* end = std::end(kAtlasGlyphs);
    const auto* found = std::lower_bound(std::begin(kAtlasGlyphs), end, codepoint,
                                         [](const AtlasGlyph& g, char32_t c) { return g.codepoint < c; });
    if (found != end && found->codepoint == codepoint) return *found;
    if (codepoint == U'?') return kAtlasGlyphs[0];
    return glyph_of(U'?');
}

std::u32string decode_utf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
        char32_t c = length == 1 ? lead : length == 2 ? (lead & 0x1fU) : length == 3 ? (lead & 0x0fU) : (lead & 0x07U);
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next >> 6) != 0x2) valid = false;
            c = (c << 6) | (next & 0x3fU);
        }
        if (!valid) {
            out.push_back(U'�');
            ++i;
            continue;
        }
        out.push_back(c);
        i += length;
    }
    return out;
}

float line_height(float size) noexcept { return static_cast<float>(kAtlasLineHeight) / 64.0F * size / kAtlasPixelSize; }
float ascender(float size) noexcept { return static_cast<float>(kAtlasAscender) / 64.0F * size / kAtlasPixelSize; }

float text_width(std::string_view text, float size) {
    float width = 0;
    for (const char32_t c : decode_utf8(text)) width += static_cast<float>(glyph_of(c).advance) / 64.0F;
    return width * size / kAtlasPixelSize;
}

float append_text(std::vector<Quad>& quads, std::string_view text, float x, float baseline, float size, Color color) {
    const float scale = size / kAtlasPixelSize;
    for (const char32_t c : decode_utf8(text)) {
        const AtlasGlyph& g = glyph_of(c);
        if (g.width > 0) {
            Quad q;
            q.glyph = true;
            q.color = color;
            q.x0 = x + static_cast<float>(g.left) * scale;
            q.y0 = baseline - static_cast<float>(g.top) * scale;
            q.x1 = q.x0 + static_cast<float>(g.width) * scale;
            q.y1 = q.y0 + static_cast<float>(g.height) * scale;
            q.u0 = static_cast<float>(g.x) / kAtlasWidth;
            q.v0 = static_cast<float>(g.y) / kAtlasHeight;
            q.u1 = static_cast<float>(g.x + g.width) / kAtlasWidth;
            q.v1 = static_cast<float>(g.y + g.height) / kAtlasHeight;
            quads.push_back(q);
        }
        x += static_cast<float>(g.advance) / 64.0F * scale;
    }
    return x;
}

void append_panel(std::vector<Quad>& quads, float x0, float y0, float x1, float y1, Color color) {
    Quad q;
    q.x0 = x0;
    q.y0 = y0;
    q.x1 = x1;
    q.y1 = y1;
    q.color = color;
    quads.push_back(q);
}

std::vector<Quad> stats_panel(const std::vector<std::string>& lines, float window_width, float window_height) {
    std::vector<Quad> quads;
    if (lines.empty() || window_width <= 0 || window_height <= 0) return quads;
    const float size = std::max(13.0F, std::round(window_height / 45.0F));
    const float pad = std::round(size * 0.6F);
    const float margin = std::round(size * 0.8F);
    const float step = std::round(line_height(size) * 1.05F);
    float widest = 0;
    for (const std::string& line : lines) widest = std::max(widest, text_width(line, size));
    const float x0 = margin;
    const float y0 = margin;
    const float x1 = std::min(window_width - margin, x0 + pad * 2 + std::ceil(widest));
    const float y1 = y0 + pad * 2 + step * static_cast<float>(lines.size());
    append_panel(quads, x0, y0, x1, y1, kPanelColor);
    float baseline = y0 + pad + std::round(ascender(size));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        append_text(quads, lines[i], x0 + pad, baseline, size, i == 0 ? kStatsTitleColor : kStatsTextColor);
        baseline += step;
    }
    return quads;
}

}  // namespace psvp::overlay
