<p align="center">
  <img src="assets/banner.png" alt="PS VulkanPlay" width="420">
</p>

# PS VulkanPlay

An open-source PlayStation Remote Play client for Linux: play your PS5 or PS4 from your desktop, laptop or
handheld. It is written from the ground up around a Vulkan renderer, with a small install and a couch-friendly
interface as goals.

## Status

Early development; nothing here is ready for players yet, and there is no streaming. What exists so far:

- finding consoles on the network, waking them from rest mode and registering with them, from the command
  line (`ps-vulkanplay --help` lists the commands)
- taking over the consoles already registered in Chiaki or chiaki-ng (`ps-vulkanplay import`)

Releases will appear on the Releases page when there is something to play.

## Building from source

You need a C++23 compiler (GCC 13 or Clang 17, or later), CMake 3.25 or later, Ninja, SDL 3.2 or later and the
Vulkan headers and loader, and OpenSSL 3 or later.

| Distribution | Packages |
|---|---|
| Arch Linux | `base-devel cmake ninja openssl sdl3 vulkan-headers vulkan-icd-loader` |
| Debian 13 | `g++ cmake ninja-build libssl-dev libsdl3-dev libvulkan-dev` |
| Alpine Linux | `build-base cmake samurai openssl-dev sdl3-dev vulkan-headers vulkan-loader-dev` |

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ps-vulkanplay
```

The same build runs on glibc and on musl; both are built and started on every change.

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
