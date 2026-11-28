# Pocket Play credits and third-party notices

Copyright © 2026 Moutaz Baaj. Original Pocket Play code is available under the GNU General Public License, version 2 or later. Each third-party component retains its copyright and license terms. The source headers and license files listed below provide the component notices. [Project license](LICENSE)

## Emulator cores and BIOS

| Component | Credits and provenance | License notices |
| --- | --- | --- |
| Gnuboy | Laguna and Gilgamesh, the contributors listed in the vendored credits, and Alex Duchesne’s Retro-Go port. The vendored source is in `src/gnuboy/`. [Original Gnuboy notice](https://sources.debian.org/src/gnuboy/1.0.3-7.1/main.c/) · [Retro-Go](https://github.com/ducalex/retro-go) | [Credits](src/gnuboy/CREDITS) · [GPL text](src/gnuboy/COPYING) |
| gpSP 1.0.0 | Exophase, notaz, David Guillen Fandos, libretro, and Retro-Go contributors. Based on Retro-Go’s ESP32 port at `16ef019`. Pocket Play adapts the frontend, memory, timing, video, and audio paths. [Pinned Retro-Go port](https://github.com/ducalex/retro-go/tree/16ef01955b2bb1e96f355a8bf76b1a4cede48598/gbsp) · [Original readme](src/gpsp/original_readme.txt) | GPL v2 or later, as stated in core source headers. [GPL text](src/gpsp/COPYING) |
| Nofrendo 3.0 | Matthew Conte and contributors, based on Retro-Go 1.46 at `856e2f7` with Pocket Play’s cartridge adapter and host changes. [Port and upstream revision](src/nofrendo/README.md) | The original core headers specify **version 2 of the GNU Library GPL**. The import preserves both upstream license files. [Credits](src/nofrendo/CREDITS) · [Library GPL v2](src/nofrendo/COPYING.LIB) · [COPYING](src/nofrendo/COPYING) |
| SMS Plus | Charles MacDonald, Eke-Eke, and contributors, from Retro-Go 1.46 at `856e2f7`. The Pocket Play port uses Nicolas Allemand’s Z80 and Mitsutaka Okazaki’s emu76489 1.22. [Port and pinned revisions](src/smsplus/README.md) | Core headers specify GPL v2 or later. The included Z80 and PSG each use MIT. [Credits](src/smsplus/CREDITS) · [Core license](src/smsplus/COPYING) · [Z80 notice](src/smsplus/cpu/LICENSE) · [PSG notice](src/smsplus/sound/LICENSE) |
| mdcore 0.1.0 | Andrew Nakas and upstream contributors. Genesis / Mega Drive core from `3db9251` with Pocket Play’s graphics, sound, controls, and saves integration. [Port and pinned revision](src/mdcore/README.md) · [Upstream 0.1.0 release](https://github.com/andrewnakas/mdcore/releases/tag/v0.1.0) | [MIT notice](src/mdcore/LICENSE) |
| ESP-DOOM | id Software, Chocolate Doom, DoomGeneric, and rear16’s ESP-DOOM port at `92bd5c3`. Pocket Play supplies the launcher, physical controls, TFT, I²S, and microSD integration. [Port and pinned revision](src/doom/README.md) | Original Doom source headers grant GPL v2 or later. See the port’s source notices and bundled GPL text for the remaining files. [GPL text](src/doom/LICENSE) |
| Open GBA BIOS replacement | Normmatt and the VBA/VBA-M team. Derived from the ReGBA copy at `7f37905`. [Upstream source](https://github.com/Nebuleon/ReGBA/tree/7f379052dc14366e622a96c7cf7fb6b5a7e918a3/bios) · [Local BIOS source and Makefile](bios-source/) · [Embedded bytes](src/gpsp/bios.h) | The BIOS import says GPL2, and individual source notices supply the terms for those files. [BIOS notice](bios-source/README.md) · [GPL text](src/gpsp/COPYING) |
| Libretro headers | RetroArch contributors. [libretro.h](src/gpsp/libretro.h) · [retro_inline.h](src/gpsp/retro_inline.h) | MIT notices are in the headers. |

## Media, scripting, and fonts

| Component | Credits and provenance | License notices |
| --- | --- | --- |
| minimp3 | lieff and contributors at `1428237`. Pocket Play adds caller-supplied decoder workspace. [Upstream source](https://github.com/lieff/minimp3/tree/142823796ab8efa55e1bf11406028f3be77ecceb) | [CC0 1.0](src/minimp3/LICENSE) |
| stb_image 2.30 | Sean Barrett and contributors at `f056911`. Pocket Play moves decoder workspace to heap. [Upstream source](https://github.com/nothings/stb/blob/f0569113c93ad095470c54bf34a17b36646bbbb5/stb_image.h) | MIT option. [Bundled notice](src/stb/stb_image.h) |
| Lua 5.4.9 | Lua.org and PUC-Rio. Pocket Play includes a subset of the standard libraries and configures the interpreter for the handheld. [Vendoring notes and upstream source](src/lua/README.md) | [MIT notice](src/lua/LICENSE) |
| Basic 8 font | Daniel Hepper and Marcel Sondaar. ASCII glyphs adapted from Retro-Go’s Basic 8 font. [Font attribution](Font8x8.h) · [Font project](https://github.com/dhepper/font8x8) | Public domain, as stated in the included attribution. |

## Build platform

The reference build uses [Arduino-ESP32 3.3.2](https://github.com/espressif/arduino-esp32/releases/tag/3.3.2). This package includes Arduino core and library code under [LGPL v2.1 and component-specific terms](https://github.com/espressif/arduino-esp32/blob/3.3.2/LICENSE.md). It builds on [ESP-IDF 5.5.1](https://github.com/espressif/esp-idf/tree/v5.5.1), whose [Apache 2.0 license and third-party terms](https://github.com/espressif/esp-idf/blob/v5.5.1/LICENSE) apply to the retained components. The reference build also links Espressif’s precompiled Wi-Fi, PHY, and BLE controller libraries. [Wi-Fi library terms](https://github.com/espressif/esp32-wifi-lib/blob/master/LICENSE) · [PHY library terms](https://github.com/espressif/esp-phy-lib/blob/master/LICENSE) · [BLE controller terms](https://github.com/espressif/esp32c3-bt-lib)

The reference build uses Espressif’s ESP-IDF IPC implementation with a larger stack for the task on the Bluetooth controller core. [Source revision and changes](src/esp32_ipc/README.md) · [Apache 2.0 license](src/esp32_ipc/LICENSE)

## Hardware and trademarks

The reference wiring is based on [AshrafHanyy’s ESP32-S3 handheld project](https://github.com/AshrafHanyy/GameBoy-ESP32-S3). Pocket Play is independently developed. Nintendo and Sega names identify the systems emulated by Pocket Play. Games and Doom IWADs are supplied separately by the player.
