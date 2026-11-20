#!/usr/bin/env bash
# Builds the Linux release artifacts into packaging/out/:
#   PS-VulkanPlay-<ver>-linux-x64.tar.gz        extract anywhere (glibc 2.35 or later: Debian, Ubuntu, Fedora, Arch, ...)
#   PS-VulkanPlay-<ver>-linux-musl-x64.tar.gz   the same for musl systems (Alpine)
#   ps-vulkanplay_<ver>_amd64.deb               system install for Debian and Ubuntu
#
#   packaging/build-release.sh            all three
#   packaging/build-release.sh glibc      the glibc tarball and the .deb only
#   packaging/build-release.sh musl       the musl tarball only
#
# The stream program is built in containers, never on this machine: against
# glibc 2.35 for the glibc artifacts, in Alpine for musl, with FFmpeg, Opus,
# OpenSSL and SDL3 linked in (packaging/native/). The app around it (.NET) is
# published with its runtime inside. Both are built from a clean copy of the
# repository's files, so nothing of this machine's build folders gets in.
# Then: packaging/release-check.sh runs every artifact before it goes anywhere.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
WHAT="${1:-all}"
VER="$(sed -nE 's/^ +VERSION ([0-9]+\.[0-9]+\.[0-9]+)$/\1/p' CMakeLists.txt | head -1)"
APP_VER="$(sed -nE 's:.*<Version>([^<]+)</Version>.*:\1:p' app/PSVulkanPlay/PSVulkanPlay.csproj)"
[ -n "$VER" ] || { echo "!! no version found in CMakeLists.txt" >&2; exit 4; }
[ "$VER" = "$APP_VER" ] || { echo "!! the stream program is $VER (CMakeLists.txt) and the app is $APP_VER (PSVulkanPlay.csproj)" >&2; exit 4; }
OUT=packaging/out
CACHE=packaging/cache
GLIBC_IMAGE=quay.io/pypa/manylinux_2_34_x86_64:latest
MUSL_IMAGE=docker.io/library/alpine:3.22
NAME="PS-VulkanPlay-$VER"
ID=io.github.codingncaffeine.PSVulkanPlay
CONTAINER="$(command -v podman || command -v docker || true)"
[ -n "$CONTAINER" ] || { echo "!! no podman or docker: the stream program cannot be built for release" >&2; exit 4; }
for tool in dotnet dpkg-deb tar gzip; do
    command -v "$tool" > /dev/null || { echo "!! $tool is missing" >&2; exit 4; }
done
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

# A full run starts from an empty folder; a one-flavor run replaces only its own artifacts.
mkdir -p "$OUT" "$CACHE"
case "$WHAT" in
    all)   rm -rf "$OUT" && mkdir -p "$OUT" ;;
    glibc) rm -rf "$OUT/native-glibc" "$OUT/$NAME-linux-x64" "$OUT/$NAME-linux-x64.tar.gz" "$OUT/ps-vulkanplay_${VER}_amd64.deb" ;;
    musl)  rm -rf "$OUT/native-musl" "$OUT/$NAME-linux-musl-x64" "$OUT/$NAME-linux-musl-x64.tar.gz" ;;
    *)     echo "usage: build-release.sh [all|glibc|musl]" >&2; exit 2 ;;
esac

# The files a release is made of: what the repository tracks, and new files that are not ignored.
rm -rf "$OUT/src"; mkdir -p "$OUT/src"
{ git ls-files -z; git ls-files -z --others --exclude-standard; } |
    while IFS= read -r -d '' f; do [ -f "$f" ] && printf '%s\0' "$f"; done |
    xargs -0 -r cp --parents -t "$OUT/src"

build_native() {   # build_native IMAGE FOLDER
    mkdir -p "$2"
    psvp_nice "$CONTAINER" run --rm --network=host -e JOBS="$PSVP_JOBS" -e VERSION="$VER" \
        -v "$PWD/$OUT/src":/src:ro,Z -v "$PWD/$2":/out:Z -v "$PWD/$CACHE":/cache:Z \
        "$1" sh /src/packaging/native/build.sh > "$2/build.log" 2>&1 || { tail -60 "$2/build.log"; return 1; }
    grep '^── ' "$2/build.log" || true
    test -x "$2/ps-vulkanplay" && test -x "$2/tools/psvp-mock-console" && test -x "$2/tools/psvp_fbcheck"
}

publish() {        # publish RID NATIVE_FOLDER FOLDER
    local app="$OUT/src/app/PSVulkanPlay"
    # Output of the other flavor's publish can leave XAML that was never compiled for this one.
    rm -rf "$app/bin" "$app/obj" "$3"
    mkdir -p "$3"
    psvp_nice dotnet publish "$app/PSVulkanPlay.csproj" -c Release -r "$1" --self-contained true \
        -p:DebugType=none -o "$3/app" -v q --nologo
    grep -c CompiledAvaloniaXaml "$3/app/PSVulkanPlay.dll" > /dev/null || { echo "!! $1: the published app has no compiled XAML" >&2; return 1; }
    test -x "$3/app/PSVulkanPlay" || { echo "!! $1: the published app has no executable" >&2; return 1; }
    # The publish leaves every file 0744: libraries and assemblies are read, not run (0644); the two
    # programs among them are run by anyone (0755).
    find "$3/app" -type d -exec chmod 755 {} +
    find "$3/app" -type f -exec chmod 644 {} +
    chmod 755 "$3/app/PSVulkanPlay"
    [ ! -f "$3/app/createdump" ] || chmod 755 "$3/app/createdump"
    install -m 755 "$2/ps-vulkanplay" "$3/ps-vulkanplay"
    cp packaging/README.txt LICENSE NOTICE packaging/THIRD-PARTY-NOTICES.md "$3/"
    cp "$2/libraries.txt" "$3/LIBRARIES.txt"
    cp "packaging/icons/hicolor/256x256/apps/$ID.png" "$3/ps-vulkanplay.png"
}

# The archive names no account of the machine it was made on: every entry belongs to 0:0.
pack() {           # pack FOLDER (under $OUT)
    tar -C "$OUT" --owner=0 --group=0 --numeric-owner --sort=name -czf "$OUT/$1.tar.gz" "$1"
}

if [ "$WHAT" = all ] || [ "$WHAT" = glibc ]; then
    echo "── the stream program for glibc ($GLIBC_IMAGE)"
    build_native "$GLIBC_IMAGE" "$OUT/native-glibc"
    echo "── the app, linux-x64"
    publish linux-x64 "$OUT/native-glibc" "$OUT/$NAME-linux-x64"
    pack "$NAME-linux-x64"

    echo "── deb"
    DEB="$OUT/debroot"
    DOC="$DEB/usr/share/doc/ps-vulkanplay"
    rm -rf "$DEB"
    mkdir -p "$DEB/DEBIAN" "$DEB/usr/bin" "$DEB/usr/lib/ps-vulkanplay" "$DEB/usr/share/applications" "$DEB/usr/share/metainfo" \
             "$DEB/usr/share/icons" "$DOC"
    install -m 755 "$OUT/$NAME-linux-x64/ps-vulkanplay" "$DEB/usr/bin/ps-vulkanplay"
    cp -a "$OUT/$NAME-linux-x64/app" "$DEB/usr/lib/ps-vulkanplay/app"
    cp packaging/README.txt "$DOC/README.txt"
    cp NOTICE "$DOC/NOTICE"
    cp packaging/THIRD-PARTY-NOTICES.md "$DOC/THIRD-PARTY-NOTICES.md"
    cp "$OUT/native-glibc/libraries.txt" "$DOC/LIBRARIES.txt"
    cp packaging/debian/copyright "$DOC/copyright"
    printf 'ps-vulkanplay (%s) unstable; urgency=medium\n\n  * Release %s. What changed:\n    https://github.com/codingncaffeine/PS-VulkanPlay/releases/tag/v%s\n\n -- PS VulkanPlay <codingncaffeine@users.noreply.github.com>  %s\n' \
        "$VER" "$VER" "$VER" "$(date -R -d "$(git log -1 --format=%cI)")" | gzip -9n > "$DOC/changelog.gz"
    cp "packaging/$ID.desktop" "$DEB/usr/share/applications/"
    cp "packaging/$ID.metainfo.xml" "$DEB/usr/share/metainfo/"
    cp -a packaging/icons/hicolor "$DEB/usr/share/icons"
    find "$DEB" -type d -exec chmod 755 {} +
    find "$DEB/usr/share" -type f -exec chmod 644 {} +
    cat > "$DEB/DEBIAN/control" <<CTRL
Package: ps-vulkanplay
Version: $VER
Section: games
Priority: optional
Architecture: amd64
Installed-Size: $(du -sk "$DEB/usr" | cut -f1)
Depends: libc6 (>= 2.35), libgcc-s1, libstdc++6, libvulkan1, libpulse0, libssl3t64 | libssl3, ca-certificates, libfontconfig1, libfreetype6, libx11-6, libice6, libsm6, libxrandr2, libxi6, libxcursor1, libxext6, libxrender1
Recommends: mesa-vulkan-drivers, libwayland-client0, libwayland-cursor0, libwayland-egl1, libxkbcommon0, libdecor-0-0, libxss1, libxfixes3, libxtst6, libudev1, libdbus-1-3, libpipewire-0.3-0t64 | libpipewire-0.3-0, libasound2t64 | libasound2, libwebkit2gtk-4.1-0, xdg-utils
Maintainer: PS VulkanPlay <codingncaffeine@users.noreply.github.com>
Homepage: https://github.com/codingncaffeine/PS-VulkanPlay
Description: Remote Play client for PS5 and PS4 consoles
 PS VulkanPlay plays a PS5 or PS4 on this computer over the network: the
 console's picture in a window drawn with Vulkan, its sound, and a controller
 with what the console sends back for it (haptics, trigger effects, the light
 bar). An app with your consoles, their settings and the steps of adding a
 console comes with it and can be used with a controller alone.
 .
 It is written from the ground up and is not affiliated with the maker of the
 consoles. FFmpeg, Opus, OpenSSL and SDL3 are linked into the stream program.
CTRL
    dpkg-deb --build --root-owner-group -Zxz "$DEB" "$OUT/ps-vulkanplay_${VER}_amd64.deb" > /dev/null
    rm -rf "$DEB"
fi

if [ "$WHAT" = all ] || [ "$WHAT" = musl ]; then
    echo "── the stream program for musl ($MUSL_IMAGE)"
    build_native "$MUSL_IMAGE" "$OUT/native-musl"
    echo "── the app, linux-musl-x64"
    publish linux-musl-x64 "$OUT/native-musl" "$OUT/$NAME-linux-musl-x64"
    pack "$NAME-linux-musl-x64"
fi

rm -rf "$OUT/src"
echo "── artifacts:"
( cd "$OUT" && ls -sh1 -- *.tar.gz *.deb 2>/dev/null && sha256sum -- *.tar.gz *.deb 2>/dev/null | tee SHA256SUMS > /dev/null )
