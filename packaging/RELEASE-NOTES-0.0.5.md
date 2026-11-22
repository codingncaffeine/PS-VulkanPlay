## What's New

- **HDR on an HDR screen** — With the "HEVC with HDR" coding, the picture now goes to the screen as HDR10 where the desktop takes it (on Linux, a Wayland session with HDR turned on, such as KDE Plasma's), and switches back to SDR for an SDR picture. Elsewhere, or with "HDR on an HDR screen" turned off in Settings, it is tone mapped to SDR as before
- **Your screen's refresh, said** — The statistics (F3) show the screen's refresh and whether it is fixed or follows the pictures (adaptive sync / VRR), and whether the picture goes out as HDR10 or SDR. When 60 pictures a second do not divide a fixed refresh (60 on a 175 Hz screen), the log says so and what shows each picture the same time: adaptive sync for the window (on KDE Plasma, Adaptive Sync "Automatic" in the display settings, with the stream full screen) or a refresh that is a multiple of 60
- **`play --hdr-output auto|off`** — The same switch for the command line

## Download

`ps-vulkanplay_0.0.5_amd64.deb` installs on Debian 12 and Ubuntu 22.04 or later. `PS-VulkanPlay-0.0.5-linux-x64.tar.gz` runs anywhere else with glibc 2.35 or later: extract it and run `./ps-vulkanplay`. `PS-VulkanPlay-0.0.5-linux-musl-x64.tar.gz` is the same for Alpine. On Arch, `ps-vulkanplay-bin` is on the AUR. Not there yet: the microphone, play from outside the home network, and DualSense haptics over Bluetooth (rumble for now). Known issue: with the console on Wi-Fi, the console can lower the picture's bitrate step by step during a long session; starting the session again brings it back.
