#!/bin/sh
# Runs INSIDE a clean container of a distribution (packaging/install-test.sh
# starts it), in two steps:
#
#   install-check.sh install ARTIFACT    with the network: installs the artifact the way a user of
#                                        this distribution would, and what the check itself needs
#                                        (a virtual screen, software Vulkan, a font)
#   install-check.sh check ARTIFACT TOOLS VERSION
#                                        without any network: every shipped binary must find its
#                                        libraries here, then packaging/smoke-test.sh plays a session
#                                        with the mock console and starts the app
# Exit 0 = everything passed.
set -u
step="$1"; artifact="$2"
. /etc/os-release

if [ "$step" = install ]; then
    # Beside the artifact: only what the check needs and no desktop has to have for the app
    # (the virtual screen, the bus, software Vulkan, a font, the system's own text shaper).
    if command -v apt-get > /dev/null; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq > /dev/null
        case "$artifact" in
            # Without the recommended packages: what the package says it NEEDS must be enough to play.
            *.deb) what="$artifact" ;;
            *) what="libvulkan1 libpulse0 libssl3 ca-certificates libfontconfig1 libfreetype6 libx11-6 libice6 libsm6 libxrandr2 libxi6 libxcursor1 libxext6 libxrender1" ;;
        esac
        # shellcheck disable=SC2086
        apt-get install -y -q --no-install-recommends $what mesa-vulkan-drivers xvfb dbus-daemon fonts-dejavu-core libharfbuzz0b > /tmp/install.log 2>&1 || { tail -25 /tmp/install.log; exit 1; }
    elif command -v dnf > /dev/null; then
        dnf install -y -q vulkan-loader pulseaudio-libs openssl-libs fontconfig freetype libX11 libICE libSM libXrandr libXi libXcursor libXext libXrender \
            libstdc++ mesa-vulkan-drivers xorg-x11-server-Xvfb dbus-daemon dejavu-sans-fonts harfbuzz tar gzip findutils diffutils > /tmp/install.log 2>&1 || { tail -25 /tmp/install.log; exit 1; }
    elif command -v pacman > /dev/null; then
        pacman -Syu --noconfirm --needed vulkan-icd-loader libpulse openssl fontconfig freetype2 libx11 libice libsm libxrandr libxi libxcursor libxext libxrender \
            vulkan-swrast xorg-server-xvfb dbus ttf-dejavu harfbuzz diffutils > /tmp/install.log 2>&1 || { tail -25 /tmp/install.log; exit 1; }
    elif command -v apk > /dev/null; then
        apk add --no-cache -q bash coreutils findutils tar vulkan-loader libpulse libssl3 libstdc++ libgcc fontconfig freetype libx11 libice libsm libxrandr libxi libxcursor libxext libxrender \
            mesa-vulkan-swrast xvfb dbus ttf-dejavu harfbuzz > /tmp/install.log 2>&1 || { tail -25 /tmp/install.log; exit 1; }
    else
        echo "FAIL no package manager this script knows"; exit 1
    fi
    command -v bash > /dev/null || { echo "FAIL no bash in this image"; exit 1; }
    case "$artifact" in
        *.deb) ;;
        *) mkdir -p /opt/ps-vulkanplay && tar -xf "$artifact" -C /opt/ps-vulkanplay --strip-components=1 || exit 1 ;;
    esac
    exit 0
fi

tools="$3"; version="$4"
problems=0
case "$artifact" in
    *.deb)
        echo "installed $(dpkg-query -W -f='${Package} ${Version}' ps-vulkanplay) on $PRETTY_NAME"
        program=/usr/bin/ps-vulkanplay; app=/usr/lib/ps-vulkanplay/app
        for f in /usr/share/applications/io.github.codingncaffeine.PSVulkanPlay.desktop \
                 /usr/share/icons/hicolor/256x256/apps/io.github.codingncaffeine.PSVulkanPlay.png \
                 /usr/share/metainfo/io.github.codingncaffeine.PSVulkanPlay.metainfo.xml /usr/share/doc/ps-vulkanplay/copyright; do
            [ -e "$f" ] || { echo "FAIL $f is missing"; problems=1; }
        done ;;
    *)
        echo "extracted $(basename "$artifact") on $PRETTY_NAME"
        program=/opt/ps-vulkanplay/ps-vulkanplay; app=/opt/ps-vulkanplay/app ;;
esac

# Every shipped binary must find its libraries, symbol versions included, on this distribution.
# Exception: .NET's LTTng trace provider, which the runtime loads only where LTTng is installed.
for f in "$program" "$app/PSVulkanPlay" "$app"/*.so; do
    case "$(basename "$f")" in libcoreclrtraceptprovider.so) continue ;; esac
    bad="$(ldd "$f" 2>&1 | grep -E "not found|Error relocating" || true)"
    if [ -n "$bad" ]; then
        echo "FAIL $(basename "$f") does not load here:"; echo "$bad" | sed 's/^/        /'
        problems=1
    fi
done
[ $problems = 1 ] || echo "ok   every shipped binary finds its libraries"

bash /repo/tools/wall.sh --check || problems=1
bash /repo/tools/wall.sh bash /repo/packaging/smoke-test.sh "$program" "$tools" "$version" || problems=1
exit $problems
