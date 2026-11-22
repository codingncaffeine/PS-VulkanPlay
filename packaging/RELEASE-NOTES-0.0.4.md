## What's New

- **Decoding on the graphics card** — Pictures are decoded with Vulkan Video and drawn straight from the card's memory; the first two seconds are checked against software decoding, sample for sample, and software takes over if they differ
- **A menu over the stream** — F1, or L1 + R1 + Options held on a controller: statistics, full screen, quit, or quit and put the console to rest
- **Reconnecting by itself** — A connection lost while playing is made again for up to a minute over the last picture, and a console that went to rest meanwhile is woken again
- **HDR pictures on an SDR desktop** — HEVC with HDR is tone mapped with the BT.2390 curve and BT.2408's reference white; each picture's brightness is measured on the graphics card and the curve follows it, smoothed, and taken at once on a scene change
- **Sharper scaling** — The picture is scaled to the window with EWA Lanczos by default (Lanczos, Catmull-Rom and bilinear in Settings), and the bands a low bitrate cuts into skies are smoothed out
- **Controller settings** — Turn trigger effects, player lights, motion and the touchpad on or off, choose the light bar's color, set a stick dead zone, and give a DualSense Edge's Fn buttons the menu, statistics or full screen
- **Straight into a session** — "Add to Steam" puts a console in Steam's library with its pictures, "Add to the app menu" puts it in the desktop's menu, and `ps-vulkanplay --play CONSOLE` or a `ps-vulkanplay://play/CONSOLE` link starts a session without the app's window
- **A picture per console** — Resolution, frame rate, bitrate and coding can be set for each console
- **Diagnostics** — A page in the app (and `ps-vulkanplay diagnose`) shows the graphics devices, whether the card can decode the stream and why not, the libraries' versions, and the last session's report
- **Full screen that covers the taskbar** — The window's maximize button and a double click on the picture go to borderless full screen
- **Easier to see** — A High Contrast look and a setting for the size of everything in the window

## Download

`ps-vulkanplay_0.0.4_amd64.deb` installs on Debian 12 and Ubuntu 22.04 or later. `PS-VulkanPlay-0.0.4-linux-x64.tar.gz` runs anywhere else with glibc 2.35 or later: extract it and run `./ps-vulkanplay`. `PS-VulkanPlay-0.0.4-linux-musl-x64.tar.gz` is the same for Alpine. On Arch, `ps-vulkanplay-bin` is on the AUR. Not there yet: HDR output to an HDR screen, the microphone, play from outside the home network, and DualSense haptics over Bluetooth (rumble for now). Known issue: with the console on Wi-Fi, the console can lower the picture's bitrate step by step during a long session; starting the session again brings it back.
