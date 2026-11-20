#!/bin/sh
# Runs INSIDE a build container (packaging/build-release.sh starts it): builds the
# stream program a release ships, into /out, and checks the file that ships.
#
#   /src     the repository's files (read-only)
#   /out     ps-vulkanplay, and tools/ with the two test programs the release
#            checks run beside it (the mock console and the screen reader);
#            those are not shipped
#   /cache   source archives, and the prefix with the built libraries
#
# The glibc build runs in manylinux_2_34 (glibc 2.34 with a backported symbol
# of 2.35, a current GCC), so the program loads on glibc 2.35 and later: Debian
# 12, Ubuntu 22.04 and everything newer; the musl build
# runs in Alpine. FFmpeg, Opus, OpenSSL, SDL3 and the C++ runtime are linked in
# (packaging/native/deps.sh). What the program still needs from the system: the
# C library, the Vulkan loader and the PulseAudio client library.
set -eu
JOBS="${JOBS:-4}"
VERSION="${VERSION:?the version the release carries}"

LIBC=glibc; command -v apk > /dev/null && LIBC=musl
if [ "$LIBC" = musl ]; then
    apk add --no-cache -q build-base cmake samurai pkgconf perl nasm curl linux-headers \
        vulkan-headers vulkan-loader-dev glslang pulseaudio-dev alsa-lib-dev pipewire-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxscrnsaver-dev libxtst-dev libxrender-dev \
        wayland-dev wayland-protocols libxkbcommon-dev libdecor-dev mesa-dev eudev-dev dbus-dev > /dev/null
else
    dnf install -y -q --enablerepo=crb perl-core nasm pkgconf-pkg-config \
        vulkan-headers vulkan-loader-devel glslang pulseaudio-libs-devel alsa-lib-devel pipewire-devel \
        libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel libXScrnSaver-devel libXtst-devel libXrender-devel \
        wayland-devel wayland-protocols-devel libxkbcommon-devel libdecor-devel mesa-libEGL-devel mesa-libGL-devel systemd-devel dbus-devel > /dev/null
fi
GEN=""; command -v ninja > /dev/null && GEN="-G Ninja"
echo "── $LIBC: $(. /etc/os-release && echo "$PRETTY_NAME"), $(c++ --version | head -1), $(cmake --version | head -1)"

# The libraries change only when their recipe does: the built prefix is kept and found again by
# the recipe's own checksum and the compiler it was built with.
key="$(cat /src/packaging/native/deps.sh | sha256sum | cut -c1-12)-$(c++ -dumpfullversion)"
prefix="/cache/deps-$LIBC-$key"
if [ ! -f "$prefix/versions.txt" ]; then
    # Built in the folder it stays in (its files name that folder); versions.txt is written last,
    # so a build that was cut short is not taken for a whole one.
    rm -rf "$prefix"
    JOBS="$JOBS" sh /src/packaging/native/deps.sh "$prefix"
    for old in /cache/deps-"$LIBC"-*; do [ "$old" = "$prefix" ] || rm -rf "$old"; done
else
    echo "── libraries: kept from an earlier build ($prefix)"
fi
sed 's/^/   /' "$prefix/versions.txt" | cut -d' ' -f1-5

mkdir -p /work
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
# shellcheck disable=SC2086
cmake -S /src -B /work/build $GEN -DCMAKE_BUILD_TYPE=Release -DPSVP_STATIC_DEPS=ON \
    -DCMAKE_PREFIX_PATH="$prefix" -DOPENSSL_ROOT_DIR="$prefix" > /work/configure.log 2>&1 || { tail -40 /work/configure.log; exit 1; }
cmake --build /work/build --parallel "$JOBS" > /work/build.log 2>&1 || { tail -60 /work/build.log; exit 1; }
# The same tests as every other build of the tree: here they run on the libraries a release carries.
( cd /work/build && ctest --output-on-failure > /work/ctest.log 2>&1 ) || { tail -60 /work/ctest.log; exit 1; }
echo "── tests: $(grep -E 'tests passed' /work/ctest.log)"

mkdir -p /out/tools
cp /work/build/ps-vulkanplay /out/ps-vulkanplay
cp /work/build/psvp-mock-console /work/build/psvp_fbcheck /out/tools/
strip --strip-unneeded /out/ps-vulkanplay /out/tools/psvp-mock-console /out/tools/psvp_fbcheck
cp "$prefix/versions.txt" /out/libraries.txt

# Checked on the file that ships. A library it needs that is not on the list is one some
# distribution does not have, or has in another version: no start there, and no build fails of it.
said="$(/out/ps-vulkanplay --version)"
[ "$said" = "ps-vulkanplay $VERSION" ] || { echo "!! the program calls itself '$said', the release is $VERSION" >&2; exit 1; }
needs="$(objdump -p /out/ps-vulkanplay | awk '$1 == "NEEDED" { print $2 }' | sort | tr '\n' ' ')"
for library in $needs; do
    case "$library" in
        libc.so.6|libm.so.6|libc.musl-x86_64.so.1|libvulkan.so.1|libpulse.so.0|ld-linux-x86-64.so.2) ;;
        *) echo "!! ps-vulkanplay needs $library from the system (all it needs: $needs)" >&2; exit 1 ;;
    esac
done
if objdump -p /out/ps-vulkanplay | grep -Eq 'RPATH|RUNPATH'; then
    echo "!! ps-vulkanplay names folders of this build to look for libraries in" >&2; exit 1
fi
if [ "$LIBC" = glibc ]; then
    newest="$(objdump -T /out/ps-vulkanplay | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    case "$newest" in
        # 2.35: _dl_find_object, which GCC's unwinder calls; the image's glibc has it backported.
        GLIBC_2.3[6-9]*|GLIBC_2.[4-9][0-9]*|GLIBC_[3-9]*) echo "!! ps-vulkanplay needs $newest: newer than the glibc 2.35 it is built for" >&2; exit 1 ;;
    esac
    echo "── ps-vulkanplay $VERSION needs: $needs| newest glibc symbol: $newest"
else
    echo "── ps-vulkanplay $VERSION needs: $needs"
fi
ls -la /out /out/tools
