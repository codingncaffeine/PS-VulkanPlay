// Makes the glyph atlas the stream window draws its text with: the printable
// Latin-1 characters of one typeface, each as a signed distance field (so that
// one atlas serves every text size), packed into one 8-bit image, and a table of
// where each glyph lies and how it is set.
//
//   glyph_atlas FONT.ttf ATLAS.bin GLYPHS.inc
//
// Run by tools/glyph-atlas/make.sh, which builds it against the system's
// FreeType. Its output is committed (assets/overlay/); the program needs
// neither FreeType nor the font file.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

namespace {

constexpr int kPixelSize = 32;   // the size the glyphs are rendered at
constexpr int kSpread = 6;       // pixels of distance on either side of an outline
constexpr int kAtlasWidth = 512;
constexpr int kGap = 1;

struct Glyph {
    unsigned codepoint = 0;
    int x = 0, y = 0, width = 0, height = 0;  // in the atlas, the distance margin included
    int left = 0, top = 0;                    // from the pen position to the bitmap's top-left (y up)
    int advance = 0;                          // in 1/64 pixel
    std::vector<std::uint8_t> pixels;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s FONT.ttf ATLAS.bin GLYPHS.inc\n", argv[0]);
        return 2;
    }
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) return 1;
    FT_Int spread = kSpread;
    FT_Property_Set(library, "bsdf", "spread", &spread);
    FT_Face face = nullptr;
    if (FT_New_Face(library, argv[1], 0, &face) != 0) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    FT_Set_Pixel_Sizes(face, 0, kPixelSize);

    std::vector<unsigned> wanted;
    for (unsigned c = 0x20; c <= 0x7e; ++c) wanted.push_back(c);
    for (unsigned c = 0xa0; c <= 0xff; ++c) wanted.push_back(c);
    for (const unsigned c : {0x2013U, 0x2014U, 0x2022U, 0x2026U, 0x2190U, 0x2191U, 0x2192U, 0x2193U}) wanted.push_back(c);
    // The program finds a glyph by a binary search: the table is in the order of the characters.
    std::sort(wanted.begin(), wanted.end());

    std::vector<Glyph> glyphs;
    for (const unsigned c : wanted) {
        const FT_UInt index = FT_Get_Char_Index(face, c);
        if (index == 0) continue;
        if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0) return 1;
        Glyph g;
        g.codepoint = c;
        g.advance = static_cast<int>(face->glyph->advance.x);
        if (face->glyph->outline.n_points > 0) {
            // A coverage bitmap first, then its distance field: the bitmap-based renderer copes with
            // outlines that cross themselves, which the outline-based one does not.
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return 1;
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != 0) return 1;
            const FT_Bitmap& bitmap = face->glyph->bitmap;
            g.width = static_cast<int>(bitmap.width);
            g.height = static_cast<int>(bitmap.rows);
            g.left = face->glyph->bitmap_left;
            g.top = face->glyph->bitmap_top;
            g.pixels.resize(static_cast<std::size_t>(g.width * g.height));
            for (int row = 0; row < g.height; ++row) {
                for (int col = 0; col < g.width; ++col) {
                    g.pixels[static_cast<std::size_t>(row * g.width + col)] = bitmap.buffer[row * bitmap.pitch + col];
                }
            }
        }
        glyphs.push_back(std::move(g));
    }

    // Rows, left to right, in the order of the characters.
    int x = kGap, y = kGap, row_height = 0;
    for (Glyph& g : glyphs) {
        if (g.width == 0) continue;
        if (x + g.width + kGap > kAtlasWidth) {
            x = kGap;
            y += row_height + kGap;
            row_height = 0;
        }
        g.x = x;
        g.y = y;
        x += g.width + kGap;
        if (g.height > row_height) row_height = g.height;
    }
    // Any height does for a texture; a multiple of 8 keeps its rows aligned.
    const int height = (y + row_height + kGap + 7) / 8 * 8;
    // 0 is "far outside": the empty atlas reads as no glyph anywhere.
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(kAtlasWidth * height), 0);
    for (const Glyph& g : glyphs) {
        for (int row = 0; row < g.height; ++row) {
            for (int col = 0; col < g.width; ++col) {
                atlas[static_cast<std::size_t>((g.y + row) * kAtlasWidth + g.x + col)] = g.pixels[static_cast<std::size_t>(row * g.width + col)];
            }
        }
    }
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(atlas.data()), static_cast<std::streamsize>(atlas.size()));

    std::ofstream out(argv[3]);
    out << "// Made by tools/glyph-atlas from " << face->family_name << " " << face->style_name << " at " << kPixelSize
        << " px, distance spread " << kSpread << " px. Do not edit.\n";
    out << "// codepoint, x, y, width, height, left, top, advance (1/64 px)\n";
    out << "inline constexpr int kAtlasWidth = " << kAtlasWidth << ";\n";
    out << "inline constexpr int kAtlasHeight = " << height << ";\n";
    out << "inline constexpr int kAtlasPixelSize = " << kPixelSize << ";\n";
    out << "inline constexpr int kAtlasSpread = " << kSpread << ";\n";
    out << "inline constexpr int kAtlasAscender = " << face->size->metrics.ascender << ";   // 1/64 px\n";
    out << "inline constexpr int kAtlasDescender = " << face->size->metrics.descender << ";\n";
    out << "inline constexpr int kAtlasLineHeight = " << face->size->metrics.height << ";\n";
    out << "inline constexpr AtlasGlyph kAtlasGlyphs[] = {\n";
    for (const Glyph& g : glyphs) {
        out << "    {0x" << std::hex << g.codepoint << std::dec << ", " << g.x << ", " << g.y << ", " << g.width << ", " << g.height << ", "
            << g.left << ", " << g.top << ", " << g.advance << "},\n";
    }
    out << "};\n";
    std::printf("%zu glyphs, atlas %dx%d\n", glyphs.size(), kAtlasWidth, height);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return 0;
}
