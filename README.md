<p align="center">
  <img src="assets/banner.png" alt="PS VulkanPlay" width="420">
</p>

# PS VulkanPlay

An open-source PlayStation Remote Play client for Linux: play your PS5 or PS4 from your desktop, laptop or
handheld. It is written from the ground up around a Vulkan renderer, with a small install and a couch-friendly
interface as goals.

## Install

Releases are on the [Releases](https://github.com/codingncaffeine/PS-VulkanPlay/releases) page.

| System | How |
|---|---|
| Debian 12, Ubuntu 22.04, or later | `sudo apt install ./ps-vulkanplay_0.0.5_amd64.deb` |
| Arch Linux | `ps-vulkanplay-bin` from the AUR |
| Any other distribution with glibc 2.35 or later | extract `PS-VulkanPlay-0.0.5-linux-x64.tar.gz`, run `./ps-vulkanplay` |
| Alpine and other musl systems | the same with `PS-VulkanPlay-0.0.5-linux-musl-x64.tar.gz` |

It needs a graphics driver with Vulkan 1.2, and PulseAudio or PipeWire for sound. To add a console: on the
console open Settings, System, Remote Play, turn Remote Play on and choose Link Device; in the app choose
Add a console, sign in with PSN and type the eight digits the console shows.

## What it does

- an app (`ps-vulkanplay` with nothing after it): your consoles as cards with Play and Wake, adding a console
  step by step (signing in with PSN gives it your account, so there is no account ID to look up), settings
  for everything a session can do, a choice of looks (High Contrast among them) and of the size of everything
  in the window, and the connected controller with its battery at the
  bottom of the window. A controller moves through it as well as a pointer does: the direction buttons or the
  left stick, accept and back, the shoulder buttons for its pages, and keys on the page for a console's PIN
- finding consoles on the network, waking them from rest mode and registering with them
  (`ps-vulkanplay --help` lists the commands)
- taking over the consoles already registered in Chiaki or chiaki-ng (`ps-vulkanplay import`)
- playing: the console's picture in a window, its sound, and what a controller does (buttons, sticks,
  triggers, touchpad, motion). The picture keeps going over lost frames and is shown evenly; the sound bridges
  short stalls of the network, and a cushion of delay can be asked for that hides the short pauses of a
  wireless network. F3 (or "Show statistics while playing" in the app) shows the picture's size and rate,
  the decoding time and the console's view of the connection over the picture. F1, or L1 + R1 + Options held
  on a controller, opens a menu over the stream: statistics, full screen, quit, or quit and put the console to rest.
  A connection lost while playing is made again by itself, for up to a minute, over the last picture; a
  console that went to rest meanwhile is woken again
- straight into a session: "Add to the app menu" on a console's card puts it in the desktop's application
  menu, and `ps-vulkanplay --play CONSOLE` or a link `ps-vulkanplay://play/CONSOLE` starts a session with the
  app's settings and no window of the app's (for launchers that start a program); "Add to Steam" puts it in
  Steam's library as a non-Steam game with its pictures (Steam closed while it is added), for Game Mode too
- decoding on the graphics card through Vulkan Video, with the pictures drawn straight from the card's memory.
  The card's first two seconds of pictures are checked against software decoding, sample for sample, and
  software takes over if they differ. "Decode on the graphics card" in the app turns it off
- HDR pictures (the "HEVC with HDR" coding) sent to the screen as HDR10 where the desktop takes it (on Linux, a
  Wayland session with HDR turned on, such as KDE Plasma's); "HDR on an HDR screen" in the app turns it off
- elsewhere, HDR pictures shown as SDR with the industry's tone mapping:
  ITU-R BT.2390's curve, BT.2408's reference white, BT.2020 colors brought into BT.709. Consoles do not say how
  bright their picture gets, so each picture's brightness is measured on the graphics card and the curve follows
  it, smoothed and taken at once on a scene change; "HDR brightness" in the app can fix it at a value instead
- the picture scaled to the window by EWA Lanczos (as chiaki-ng does), or by Lanczos, Catmull-Rom or bilinear,
  and the bands a low bitrate cuts skies into smoothed out ("Scaling" and "Smooth out banding" in the app)
- a DualSense on USB gets what a PS5 sends for it: haptics on the controller's own sound device, the effects
  of its adaptive triggers, the light bar's color; other controllers get rumble. The app's Controller settings
  turn trigger effects, player lights, motion and the touchpad on or off, set the light bar's color, a stick
  dead zone, and what a DualSense Edge's Fn buttons do (the menu over the stream, statistics, full screen)

- the screen's refresh said in the log and the statistics (F3): fixed, or following the pictures (adaptive
  sync / VRR, which the desktop turns on for a full screen window: on KDE Plasma, Adaptive Sync "Automatic")

Not there yet: the microphone, and play from outside the home network.

## Building from source

You need a C++23 compiler (GCC 13 or Clang 17, or later), CMake 3.25 or later, Ninja, pkg-config, SDL 3.2 or
later, FFmpeg 7 or later (libavcodec), Opus, OpenSSL 3 or later, the Vulkan headers and loader, and glslang to
compile the shaders. With the PulseAudio client library (which PipeWire serves as well) the program also sets
the volume of a controller's haptics; without it, it builds the same less that. Decoding on the graphics card
needs FFmpeg 7.1 or later built with Vulkan; with an older one, pictures are decoded in software. The app is a .NET program
of its own (`app/`) that runs the stream program: it needs the .NET 10 SDK, and the stream program builds
and plays without it.

| Distribution | Packages |
|---|---|
| Arch Linux | `base-devel cmake ninja openssl ffmpeg opus libpulse sdl3 vulkan-headers vulkan-icd-loader glslang` |
| Debian 13 | `g++ cmake ninja-build pkg-config libssl-dev libavcodec-dev libavutil-dev libopus-dev libpulse-dev libsdl3-dev libvulkan-dev glslang-tools` |
| Alpine Linux | `build-base cmake samurai pkgconf openssl-dev ffmpeg-dev opus-dev pulseaudio-dev sdl3-dev vulkan-headers vulkan-loader-dev glslang` |

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ps-vulkanplay play CONSOLE

dotnet build app/PSVulkanPlay -c Release      # the app; put its output in a folder "app" beside ps-vulkanplay
```

`tools/build.sh` followed by `tools/build-app.sh` does both and leaves the program and the app in `bin/`.

The same build runs on glibc and on musl; both are built and started on every change. The release packages
are made by `packaging/build-release.sh` and checked by `packaging/release-check.sh`, which installs each one
in containers of the distributions it is for and plays a session there against a stand-in console.

## License

PS VulkanPlay is free software under the [GNU Affero General Public License, version 3](LICENSE).

This project is not affiliated with, endorsed or certified by Sony Interactive Entertainment. PlayStation, PS5
and PS4 are trademarks of Sony Interactive Entertainment Inc.

## Credits

PS VulkanPlay exists because of the people who worked out how Remote Play talks and shared it as free software:

- [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), which began as chiaki4deck, by Street Pea and its
  contributors: the client this project sets out to follow, and the reference our protocol code is tested
  against.
- [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki) by Florian Märkl and its contributors: the original
  open-source Remote Play client, where that research started.

The code here is our own; see [NOTICE](NOTICE) for how the two projects are used.
