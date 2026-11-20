#!/bin/sh
# Runs INSIDE a build container (packaging/native/build.sh starts it): builds the
# four libraries a release links into the stream program, as static libraries
# under the prefix given, from their makers' source archives at pinned versions.
#
#   packaging/native/deps.sh PREFIX
#
#   OpenSSL   libcrypto only is used: the keys and ciphers of the protocol
#   Opus      the stream's sound
#   FFmpeg    libavcodec and libavutil with the H.264 and HEVC decoders and
#             nothing else (LGPL: no GPL part is switched on)
#   SDL3      the window, sound output and controllers. It opens the desktop's
#             own libraries (X11, Wayland, PulseAudio, PipeWire, udev, D-Bus)
#             when it runs, so none of them becomes a library the program needs
#
# Distributions ship these four in versions that do not load each other's
# programs (and Debian 12 and Ubuntu up to 24.04 have no SDL3 at all); built in,
# one program runs on all of them. /cache keeps the archives.
set -eu
PREFIX="$1"
JOBS="${JOBS:-4}"
WORK="${WORK:-/work/deps}"

OPENSSL_VER=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
OPUS_VER=1.6.1
OPUS_SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
FFMPEG_VER=9.1
FFMPEG_SHA256=1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433
SDL_VER=3.4.20
SDL_SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f

fetch() {   # fetch FILE SHA256 URL
    [ -f "/cache/$1" ] || curl -fsSL -o "/cache/$1.part" "$3"
    [ -f "/cache/$1" ] || mv "/cache/$1.part" "/cache/$1"
    echo "$2  /cache/$1" | sha256sum -c - > /dev/null || { echo "!! /cache/$1 is not the archive that was pinned" >&2; exit 1; }
}
quiet() {   # quiet LOG COMMAND...: what it said is shown only when it fails
    log="$1"; shift
    "$@" > "$log" 2>&1 || { tail -40 "$log"; echo "!! failed: $*" >&2; exit 1; }
}

mkdir -p "$WORK" "$PREFIX"
GEN=""; command -v ninja > /dev/null && GEN="-G Ninja"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

echo "── OpenSSL $OPENSSL_VER"
fetch "openssl-$OPENSSL_VER.tar.gz" "$OPENSSL_SHA256" "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
tar -xzf "/cache/openssl-$OPENSSL_VER.tar.gz" -C "$WORK"
( cd "$WORK/openssl-$OPENSSL_VER"
  # Its settings file is looked for in a folder nothing installs: the program's ciphers do not
  # depend on what a distribution wrote into its own.
  quiet "$WORK/openssl-configure.log" ./Configure linux-x86_64 no-shared no-tests no-apps no-docs no-engine no-legacy no-async \
      --prefix="$PREFIX" --libdir=lib --openssldir=/usr/lib/ps-vulkanplay/ssl
  quiet "$WORK/openssl-build.log" make -j "$JOBS" build_libs
  quiet "$WORK/openssl-install.log" make install_dev )

echo "── Opus $OPUS_VER"
fetch "opus-$OPUS_VER.tar.gz" "$OPUS_SHA256" "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
tar -xzf "/cache/opus-$OPUS_VER.tar.gz" -C "$WORK"
# shellcheck disable=SC2086
quiet "$WORK/opus-configure.log" cmake -S "$WORK/opus-$OPUS_VER" -B "$WORK/opus-build" $GEN -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DBUILD_TESTING=OFF
quiet "$WORK/opus-build.log" cmake --build "$WORK/opus-build" --parallel "$JOBS"
quiet "$WORK/opus-install.log" cmake --install "$WORK/opus-build"

echo "── FFmpeg $FFMPEG_VER (H.264 and HEVC decoders only)"
fetch "ffmpeg-$FFMPEG_VER.tar.xz" "$FFMPEG_SHA256" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
tar -xJf "/cache/ffmpeg-$FFMPEG_VER.tar.xz" -C "$WORK"
( cd "$WORK/ffmpeg-$FFMPEG_VER"
  quiet "$WORK/ffmpeg-configure.log" ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" \
      --enable-static --disable-shared --enable-pic --disable-autodetect --disable-programs --disable-doc --disable-debug \
      --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter --disable-network \
      --disable-everything --enable-decoder=h264 --enable-decoder=hevc --enable-parser=h264 --enable-parser=hevc
  # Decoding without the processor's own instructions, or on one core, is several times slower:
  # a build that came out that way is not a release build.
  for have in HAVE_X86ASM HAVE_PTHREADS; do
      grep -q "^#define $have 1" config.h || { echo "!! FFmpeg was configured without $have" >&2; exit 1; }
  done
  quiet "$WORK/ffmpeg-build.log" make -j "$JOBS"
  quiet "$WORK/ffmpeg-install.log" make install )

echo "── SDL $SDL_VER"
fetch "SDL3-$SDL_VER.tar.gz" "$SDL_SHA256" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz"
tar -xzf "/cache/SDL3-$SDL_VER.tar.gz" -C "$WORK"
# shellcheck disable=SC2086
quiet "$WORK/sdl-configure.log" cmake -S "$WORK/SDL3-$SDL_VER" -B "$WORK/sdl-build" $GEN -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
    -DSDL_CAMERA=OFF -DSDL_GPU=OFF
# What the program is for must be in the library: both window systems, the three ways sound
# leaves a Linux desktop, controllers over the raw HID interface (a DualSense's triggers, lights
# and motion go that way) and the two system services it finds devices and settings through.
config="$(find "$WORK/sdl-build" -name SDL_build_config.h | head -1)"
[ -n "$config" ] || { echo "!! SDL's build settings file was not found" >&2; exit 1; }
for have in SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_WAYLAND SDL_VIDEO_VULKAN SDL_AUDIO_DRIVER_PULSEAUDIO \
            SDL_AUDIO_DRIVER_PIPEWIRE SDL_AUDIO_DRIVER_ALSA SDL_JOYSTICK_HIDAPI SDL_JOYSTICK_LINUX SDL_HAPTIC_LINUX \
            HAVE_LIBUDEV_H HAVE_DBUS_DBUS_H HAVE_LIBDECOR_H SDL_VIDEO_DRIVER_X11_XCURSOR SDL_VIDEO_DRIVER_X11_XINPUT2 \
            SDL_VIDEO_DRIVER_X11_XRANDR SDL_VIDEO_DRIVER_X11_XFIXES SDL_VIDEO_DRIVER_X11_XSCRNSAVER; do
    grep -Eq "^#define $have( 1)?\$" "$config" || { echo "!! SDL was configured without $have" >&2; exit 1; }
done
# Each of those libraries is opened by name when the program runs; one that SDL would link
# instead becomes a library the program cannot start without.
for dynamic in SDL_VIDEO_DRIVER_X11_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC \
               SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC SDL_AUDIO_DRIVER_ALSA_DYNAMIC SDL_UDEV_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR; do
    grep -Eq "^#define $dynamic \"" "$config" || { echo "!! SDL would link, not open, the library of $dynamic" >&2; exit 1; }
done
quiet "$WORK/sdl-build.log" cmake --build "$WORK/sdl-build" --parallel "$JOBS"
quiet "$WORK/sdl-install.log" cmake --install "$WORK/sdl-build"

rm -rf "$WORK"
{
    echo "OpenSSL $OPENSSL_VER   https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
    echo "Opus $OPUS_VER   https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
    echo "FFmpeg $FFMPEG_VER   https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
    echo "SDL $SDL_VER   https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz"
} > "$PREFIX/versions.txt"
