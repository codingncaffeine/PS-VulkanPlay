#!/usr/bin/env bash
# Installs what a build from source and its checks need, on the distribution it
# runs on (a container or a CI job; it needs root).
#
#   tools/ci/deps.sh [clang] [app]
#       clang: also the other compiler and standard library
#       app:   also what the app (app/, .NET) is built and started with: the .NET SDK, and the
#              libraries its window needs on a virtual screen
set -euo pipefail
clang=0 app=0
for word in "$@"; do
    case "$word" in
        clang) clang=1 ;;
        app) app=1 ;;
        *) echo "deps: unknown word $word" >&2; exit 2 ;;
    esac
done

if command -v apk >/dev/null 2>&1; then
    packages=(build-base cmake samurai pkgconf openssl-dev ffmpeg-dev opus-dev pulseaudio-dev sdl3-dev vulkan-headers vulkan-loader-dev glslang mesa-vulkan-swrast xvfb xprop dbus iproute2)
    [ $clang = 1 ] && packages+=(clang libc++-dev)
    # harfbuzz: with the system's own text shaper installed, an app that binds its bundled one to
    # it dies at its first window; the check must be able to see that.
    [ $app = 1 ] && packages+=(dotnet10-sdk fontconfig ttf-dejavu harfbuzz libx11 libice libsm libxrandr libxi libxcursor libxext libxrender)
    apk add --no-cache -q "${packages[@]}"
elif command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    packages=(g++ cmake ninja-build pkg-config libssl-dev libavcodec-dev libavutil-dev libopus-dev libpulse-dev libsdl3-dev libvulkan-dev glslang-tools mesa-vulkan-drivers xvfb x11-utils dbus-daemon iproute2 ca-certificates)
    [ $clang = 1 ] && packages+=(clang libc++-dev libc++abi-dev)
    [ $app = 1 ] && packages+=(curl libicu76 zlib1g fontconfig fonts-dejavu-core libharfbuzz0b libx11-6 libice6 libsm6 libxrandr2 libxi6 libxcursor1 libxext6 libxrender1)
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends "${packages[@]}" > /dev/null
    if [ $app = 1 ] && ! command -v dotnet >/dev/null 2>&1; then
        # Debian has no package of the .NET SDK: its maker's own installer puts it under /usr/share/dotnet.
        curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
        bash /tmp/dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet > /dev/null
        ln -sf /usr/share/dotnet/dotnet /usr/bin/dotnet
        rm -f /tmp/dotnet-install.sh
    fi
elif command -v pacman >/dev/null 2>&1; then
    packages=(base-devel cmake ninja openssl ffmpeg opus libpulse sdl3 vulkan-headers vulkan-icd-loader glslang vulkan-swrast xorg-server-xvfb xorg-xprop dbus iproute2)
    [ $clang = 1 ] && packages+=(clang libc++)
    [ $app = 1 ] && packages+=(dotnet-sdk fontconfig ttf-dejavu harfbuzz libx11 libice libsm libxrandr libxi libxcursor libxext libxrender)
    pacman -Sy --needed --noconfirm "${packages[@]}" > /dev/null
else
    echo "deps: no apk, apt-get or pacman here" >&2
    exit 2
fi
