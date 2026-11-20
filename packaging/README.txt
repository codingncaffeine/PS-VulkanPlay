PS VulkanPlay
=============

A Remote Play client for PS5 and PS4 consoles: play your console on this
computer over your home network.

Start it
--------

  From the tarball:   ./ps-vulkanplay
  From a package:     PS VulkanPlay in the applications menu, or ps-vulkanplay

That opens the app: your consoles, adding one, settings. A controller moves
through it as well as a pointer does.

Add a console
-------------

1. On the console: Settings, System, Remote Play, turn on Enable Remote Play,
   then Link Device. It shows an 8-digit number.
2. In the app: Add a console, sign in with your PSN account (that gives the
   console your account), then type the number.

The console and this computer must be on the same network. Play starts a
session; Wake turns on a console that is in rest mode.

What it needs
-------------

  - a graphics driver with Vulkan 1.2
  - PulseAudio or PipeWire for sound
  - X11 or Wayland

The .deb names its packages. For the tarball on other distributions:
the Vulkan loader, the PulseAudio client library, fontconfig, and the X11
libraries libX11, libICE, libSM, libXrandr, libXi, libXcursor, libXext and
libXrender. With WebKitGTK 4.1 installed the sign-in page opens in a window of
the app; without it, in your browser.

A DualSense gets haptics, trigger effects and the light bar's color when it is
connected by USB; over Bluetooth it gets rumble for now.

From a terminal
---------------

  ps-vulkanplay --help               the commands
  ps-vulkanplay discover             the consoles on the network
  ps-vulkanplay play CONSOLE         a session without the app

Settings are kept in ~/.config/ps-vulkanplay, logs in
~/.local/state/ps-vulkanplay/logs.

License
-------

GNU Affero General Public License, version 3 (LICENSE). The source code:
https://github.com/codingncaffeine/PS-VulkanPlay

What is built in, and under which licenses: THIRD-PARTY-NOTICES.md, and
LIBRARIES.txt for the exact versions and where their source comes from.

PS VulkanPlay is not affiliated with, endorsed or certified by Sony
Interactive Entertainment. PlayStation, PS5 and PS4 are trademarks of Sony
Interactive Entertainment Inc.
