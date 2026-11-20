#!/usr/bin/env bash
# Builds the AUR package from the artifacts in packaging/out/ and this checkout's
# desktop entry, metainfo and icons, before the release they will be downloaded
# from exists: makepkg uses a source file it already finds under the recipe's
# name. Then runs the package's contents with packaging/smoke-test.sh behind the
# test wall.
#
#   packaging/aur/local-test.sh [FOLDER]
#     FOLDER: a folder holding an earlier version's downloaded sources (for
#             example a copy of ~/.cache/yay/ps-vulkanplay-bin). They are copied
#             in first, so the build runs the way an upgrade does.
#
# Nothing is installed. Run packaging/aur/checksums.sh first.
set -euo pipefail
cd "$(dirname "$0")/../.."
id=io.github.codingncaffeine.PSVulkanPlay
ver="$(grep -oPm1 '(?<=^pkgver=).+' packaging/aur/PKGBUILD)"
work=packaging/out/aur
rm -rf "$work"; mkdir -p "$work"
if [ -n "${1:-}" ]; then
    find "$1" -maxdepth 1 -type f ! -name PKGBUILD ! -name .SRCINFO -exec cp -t "$work" {} +
    echo "── earlier sources copied in: $(ls "$work" | wc -l) files"
fi
cp packaging/aur/PKGBUILD "$work/"
cp "packaging/out/PS-VulkanPlay-$ver-linux-x64.tar.gz" "$work/"
cp "packaging/$id.desktop" "$work/ps-vulkanplay-$ver.desktop"
cp "packaging/$id.metainfo.xml" "$work/ps-vulkanplay-$ver.metainfo.xml"
for size in 16 24 32 48 64 128 256; do cp "packaging/icons/hicolor/${size}x${size}/apps/$id.png" "$work/ps-vulkanplay-$ver-$size.png"; done
# Every source name must carry the version, or an upgrade reuses the old file.
if sed -n '/^source=/,/^sha256sums/p' packaging/aur/PKGBUILD | grep -E '^\s*("|source=\(")' | grep -v 'pkgver'; then
    echo "!! a source name without \$pkgver (above)" >&2; exit 1
fi
( cd "$work" && makepkg -f --nodeps > makepkg.log 2>&1 ) || { tail -20 "$work/makepkg.log"; echo "AUR LOCAL TEST FAILED: makepkg"; exit 1; }
pkg="$(ls "$work"/ps-vulkanplay-bin-"$ver"-*.pkg.tar.zst)"
echo "── built $(basename "$pkg")"
rm -rf "$work/root"; mkdir -p "$work/root"
tar -xf "$pkg" -C "$work/root"
tools/wall.sh packaging/smoke-test.sh "$work/root/usr/bin/ps-vulkanplay" packaging/out/native-glibc/tools "$ver"
