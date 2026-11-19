#!/usr/bin/env bash
# Installs what a build from source and its checks need, on the distribution it
# runs on (a container or a CI job; it needs root).
#
#   tools/ci/deps.sh [clang]     clang: also the other compiler and standard library
set -euo pipefail
clang=0; [ "${1:-}" = clang ] && clang=1

if command -v apk >/dev/null 2>&1; then
    packages=(build-base cmake samurai sdl3-dev vulkan-headers vulkan-loader-dev mesa-vulkan-swrast xvfb dbus)
    [ $clang = 1 ] && packages+=(clang libc++-dev)
    apk add --no-cache -q "${packages[@]}"
elif command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    packages=(g++ cmake ninja-build libsdl3-dev libvulkan-dev mesa-vulkan-drivers xvfb dbus-daemon ca-certificates)
    [ $clang = 1 ] && packages+=(clang libc++-dev libc++abi-dev)
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends "${packages[@]}" > /dev/null
elif command -v pacman >/dev/null 2>&1; then
    packages=(base-devel cmake ninja sdl3 vulkan-headers vulkan-icd-loader vulkan-swrast xorg-server-xvfb dbus)
    [ $clang = 1 ] && packages+=(clang libc++)
    pacman -Sy --needed --noconfirm "${packages[@]}" > /dev/null
else
    echo "deps: no apk, apt-get or pacman here" >&2
    exit 2
fi
