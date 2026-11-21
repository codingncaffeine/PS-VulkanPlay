## What's New

The first pre-release of PS VulkanPlay: a Remote Play client for PS5 and PS4 consoles, written from the ground up, for Linux.

- **The app** — Your consoles as cards with Play and Wake, settings for everything a session can do, and six looks to choose from
- **Add a console in three steps** — Pick it from the network, sign in with PSN, type the PIN the console shows
- **Use it from the couch** — A controller moves through the whole app: the direction buttons or the left stick, accept and back, the shoulder buttons for its pages
- **Play at up to 1080p and 60 pictures a second** — H.264 or HEVC in a window drawn with Vulkan, with the console's sound
- **A picture that keeps going** — Lost frames do not freeze it, pictures are shown evenly, and an optional cushion of delay hides the short pauses of a wireless network
- **DualSense features** — Haptics, adaptive trigger effects and the light bar's color over USB, touchpad and motion, and the battery level in the app; the back buttons of a DualSense Edge can each be given a button
- **Wake and rest** — Wake a console from rest mode when Play is pressed, and send it back there when the session ends
- **Take over from Chiaki** — `ps-vulkanplay import` brings along the consoles registered in Chiaki or chiaki-ng
- **Packages** — A `.deb` for Debian and Ubuntu, a tarball for any other glibc distro, a tarball for Alpine and other musl systems, and `ps-vulkanplay-bin` on the AUR

## Download

`ps-vulkanplay_0.0.3_amd64.deb` installs on Debian 12 and Ubuntu 22.04 or later. `PS-VulkanPlay-0.0.3-linux-x64.tar.gz` runs anywhere else with glibc: extract it and run `./ps-vulkanplay`. `PS-VulkanPlay-0.0.3-linux-musl-x64.tar.gz` is the same for Alpine. It plays over the home network, and was developed and played on a PS5. Not there yet: decoding on the graphics card, HDR output, the microphone, a menu over the stream, and DualSense haptics over Bluetooth (rumble for now). Known issue: with the console on Wi-Fi, the console can lower the picture's bitrate step by step during a long session; starting the session again brings it back.
