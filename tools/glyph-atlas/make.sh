#!/usr/bin/env bash
# Makes assets/overlay/: the glyph atlas the stream window draws its text with,
# from Inter (the app's typeface, SIL Open Font License 1.1), and the table of
# its glyphs. Needs a C++ compiler, FreeType 2.11 or later and unzip; downloads
# the font's release archive once (checked against its known checksum).
#
#   tools/glyph-atlas/make.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh
version=4.1
sha256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
cache=packaging/cache/inter
work=build/glyph-atlas
mkdir -p "$cache" "$work" assets/overlay
archive="$cache/Inter-$version.zip"
if [ ! -f "$archive" ]; then
    curl -fsSL -o "$archive.part" "https://github.com/rsms/inter/releases/download/v$version/Inter-$version.zip"
    mv "$archive.part" "$archive"
fi
echo "$sha256  $archive" | sha256sum -c - > /dev/null
unzip -o -q -j "$archive" "extras/ttf/Inter-Medium.ttf" LICENSE.txt -d "$work"
psvp_nice c++ -std=c++23 -O2 -Wall -Wextra -o "$work/glyph_atlas" tools/glyph-atlas/glyph_atlas.cpp $(pkg-config --cflags --libs freetype2)
"$work/glyph_atlas" "$work/Inter-Medium.ttf" assets/overlay/inter-medium-atlas.bin src/overlay/inter_glyphs.inc
cp "$work/LICENSE.txt" assets/overlay/Inter-LICENSE.txt
