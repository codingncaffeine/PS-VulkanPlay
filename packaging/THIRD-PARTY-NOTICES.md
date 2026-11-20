# What PS VulkanPlay's packages contain besides its own code

PS VulkanPlay itself is licensed under the GNU Affero General Public License, version 3 (see `LICENSE`).
Its packages also carry the work of others, each under its own license. `LIBRARIES.txt` in the package
names the exact versions of the libraries linked into the stream program and the addresses their source
archives were taken from; the recipe they were built with is `packaging/native/deps.sh` in the source
repository.

## Linked into the stream program (`ps-vulkanplay`)

| Library | What it does here | License |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil) | decodes the picture (H.264, HEVC) | LGPL 2.1 or later |
| [Opus](https://opus-codec.org) | decodes the sound | BSD 3-Clause |
| [OpenSSL](https://www.openssl.org) (libcrypto) | the protocol's keys and ciphers | Apache License 2.0 |
| [SDL](https://libsdl.org) 3 | the window, sound output, controllers | zlib |
| The GNU C++ runtime (libstdc++, libgcc) | | GPL 3 with the GCC Runtime Library Exception |
| musl (musl packages only: nothing of it is linked in, it is the system's) | | MIT |

FFmpeg is built with nothing but its H.264 and HEVC decoders and their parsers, and without any of its
GPL or non-free parts. It is used under the LGPL: the stream program's own source is public, so the
program can be built again with another build of FFmpeg (`README.md`, "Building from source", links
whatever FFmpeg the system has).

## The app (`app/`)

| Component | License |
|---|---|
| [.NET runtime](https://github.com/dotnet/runtime) | MIT |
| [Avalonia](https://avaloniaui.net) and its WebView control | MIT |
| [SkiaSharp](https://github.com/mono/SkiaSharp) with [Skia](https://skia.org) | MIT; BSD 3-Clause |
| [HarfBuzzSharp](https://github.com/mono/SkiaSharp) with [HarfBuzz](https://harfbuzz.github.io) | MIT |
| [MicroCom](https://github.com/kekekeks/MicroCom) | MIT |
| [Tmds.DBus.Protocol](https://github.com/tmds/Tmds.DBus) | MIT |
| [Inter](https://rsms.me/inter/) (the app's typeface) | SIL Open Font License 1.1 |

## Where the knowledge of the protocol comes from

See `NOTICE`: Chiaki and chiaki-ng, both under the GNU Affero General Public License, version 3. No
source file of either is part of PS VulkanPlay.
