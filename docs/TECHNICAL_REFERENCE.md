# Pocket Play technical reference

[Build and upload](../BUILDING.md#build-and-upload) · [Hardware](HARDWARE.md) · [Player guide](../README-USER.md)

## Code and emulator lineage

| System | Core |
| --- | --- |
| Game Boy and Game Boy Color | [Gnuboy](../src/gnuboy/), from the Retro-Go port |
| Game Boy Advance | [gpSP](../src/gpsp/GbaCore.c), from [Retro-Go’s ESP32 port](https://github.com/ducalex/retro-go/tree/16ef01955b2bb1e96f355a8bf76b1a4cede48598/gbsp) |
| NES | [Nofrendo 3.0](../src/nofrendo/README.md) |
| Master System and Game Gear | [SMS Plus](../src/smsplus/README.md) |
| Genesis / Mega Drive | [mdcore](../src/mdcore/README.md) |
| Doom | [ESP-DOOM port](../src/doom/README.md) |

`PocketPlay.ino` contains the launcher, game loop, settings, and save handling. `Display.cpp` drives the ILI9341. `MusicPlayer.cpp` and `PhotoViewer.cpp` handle SD media. `UsbControls.cpp`, `BluetoothControls.cpp`, and `HidGamepad.cpp` handle external input. Battery sampling feeds the charge estimate in `BatteryGauge.cpp` and diagnostic captures in `DeviceLog.cpp`.

[`WifiTransfer.cpp`](../WifiTransfer.cpp) runs the local HTTP server and handles SD uploads in transfer mode. [`FirmwareUpdate.cpp`](../FirmwareUpdate.cpp) validates the Pocket Play app image and stages it in the inactive OTA slot. [`DoomLauncher.cpp`](../DoomLauncher.cpp) connects the Doom engine to the display, physical controls, I²S, and microSD.

## Files and formats

Use the [player guide’s microSD layout](../README-USER.md#set-up-the-microsd-card). The firmware also scans the card root for ROMs, with up to 512 entries per system. Size limits are 4 MiB for GB/GBC and NES, 32 MiB for GBA, and 2 MiB of cartridge data for Master System and Game Gear. NES uses iNES cartridges with supported mappers. Master System and Game Gear accept common 512-byte copier headers and use PSG audio. Genesis cartridges can be up to 6 MiB. Genesis allocates cartridge and machine memory in PSRAM when a game opens. Doom requires 8 MiB of PSRAM for its dedicated boot mode and its IWAD can be up to 32 MiB. The bundled open GBA BIOS can be overridden with a readable 16 KiB dump at `/retro-go/bios/gba_bios.bin`.

| Beside the ROM | Contents |
| --- | --- |
| `.sav` | Cartridge save data |
| `.state`, `.state2`, `.state3` | Save slots 1–3 |
| `.state.latest` | Slot selected by Continue |
| `.profile` | Per-game settings |

Game covers use uncompressed 24-bit BMPs up to 1024 × 1024. [Cover setup](../README-USER.md#play-a-game)

### Photos

Photos shows the first frame of a GIF and the saved composite of a PSD. Each album lists up to 512 entries. In a PSRAM build, the limits are 6 MiB per file and 1,048,576 source pixels. Without PSRAM they are 256 KiB and 32,768 pixels. [Photo controls](../README-USER.md#view-photos)

### Music player

Music accepts mono or stereo MP3 and 16-bit PCM WAV. WAV rates in kHz are 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48, 88.2, and 96. A folder can list up to 512 tracks and 512 subfolders. The player skips dot-prefixed entries. [Music controls](../README-USER.md#play-music)

Background playback runs in supported menus. Starting a game pauses the track while the emulator uses I²S. Playback resumes when you quit. A card rescan from Home or a game library ends the music session. With the display off in Music, tap a button to wake the panel and tap again to take the action. [Music and audio code](../MusicPlayer.cpp)

### Diagnostic Log

Captures are buffered in RAM at a target of 10 samples per second and saved as versioned `.ppl` files in `/logs`. The latest completed capture is available in memory if the card write fails. Type `log` over UART at 115200 baud for CSV. Current, battery power, and USB charging status export as `NA`. [Record and review a capture](../README-USER.md#record-a-diagnostic-log)

## Wi-Fi transfer and firmware updates

Transfer mode consumes a one-time boot request and starts a password-protected access point at `http://192.168.4.1/`. The reference build uses the Arduino-ESP32 **3.3.2** SoftAP and its DHCP service. The normal launcher, Bluetooth, audio, and ROM scanning are bypassed during this session. The HTTP server handles one transfer at a time. SD uploads fill a 4 KiB buffer, yield to FreeRTOS between writes, and publish the destination only after syncing and closing the temporary file. The temporary file is cleared after a handled failure or when the next transfer session starts. Local controls and the idle timer wait until the active upload or OTA request ends before restarting. [Reference board options](../BUILDING.md#build-and-upload)

The 16 MB reference partition table has OTA data at `0xe000` and two 3 MiB app slots at `0x10000` and `0x310000`. The app-only image leaves the layout in place. USB installation supplies the bootloader, partition table, initial OTA data, and app. [Pinned 3.3.2 partition table](https://raw.githubusercontent.com/espressif/arduino-esp32/3.3.2/tools/partitions/app3M_fat9M_16MB.csv) · [USB packaging and recovery](../BUILDING.md#package-firmware-for-usb-and-wi-fi)

Browser and microSD updates share the installer. Browser **Install firmware** streams the app to the inactive slot. **Save to microSD** writes a file to `/updates/` for later selection under **Settings → About → Update Software**, which scans that folder and the card root. Before writing, the installer checks the image prefix for ESP32-S3 metadata and Pocket Play's embedded wiring identifier. ESP-IDF validates the completed image and Pocket Play checks its length before selecting the new boot slot.

The console shows a centered installation screen. Its percentage follows successful flash writes, with 100% reserved for a validated image selected for boot. The browser displays a separate upload bar. Wi-Fi installation waits for **Restart console**. microSD installation restarts automatically. On builds with rollback enabled, startup checks configuration, display, and PSRAM before accepting a pending OTA boot. Recover over USB if the console cannot boot. [Wi-Fi instructions](../README-USER.md#update-firmware-over-wi-fi) · [microSD instructions](../README-USER.md#update-firmware-from-microsd)

## Applications

`.ppapp` packages are SDK-versioned Lua source files. The browser can install them in `/apps/` or stage them in `/app-install/`. **Install Application** in About reads staged packages and the card root. Installation copies the package to the card without executing its source. Applications opens one package at a time on the main loop, where Pocket Play owns display and audio. [App API and limits](APP_SDK.md)

## GBA settings and trade-offs

Open **Settings → Emulation → Game Boy Advance** and try **Native** and **Auto** first. The ESP32-S3 port runs the ARM/Thumb interpreter and uses internal RAM for hot buffers when possible. The display task runs on the other core and can skip a presentation when busy. Larger ROMs page from microSD into a PSRAM cache.

| Setting | Options |
| --- | --- |
| GBA Display | Native centers the 240 × 160 image. Full scales to 320 × 240. |
| GBA Frameskip | None renders each emulated frame. Auto, Light, and High can skip rendering when behind schedule. |
| GBA Transfer | DMA by default, with a 40 MHz clock cap. Arduino SPI is also available. |
| TFT Speed | 20, 40, 60, or 80 MHz requested. See the serial log for the active transfer and clock. |
| Game Sound | Off on a fresh install. Turning it off bypasses GBA mixer work. Volume adjusts PCM level. |

At 40 MHz, the native frame takes about 15.36 ms to transfer and the full frame takes 30.72 ms, before overhead.

## Genesis performance settings

Open **Settings → Emulation → Sega Genesis** from Home or from the in-game pause menu. Settings are saved automatically and take effect when a game starts or resumes. An enabled game profile can override display size.

| Setting | Options |
| --- | --- |
| **Display Size** | **Native** centers the original image and is the default for Genesis. **Fit** keeps the aspect ratio. **Full** fills the panel. |
| **Frameskip** | **Auto** can skip one render when the core is behind or the display is busy. **None** renders every emulated frame. **Light** renders every second frame and **High** renders every fourth. The emulated CPUs and audio chips still advance. |

Start with **Native** and **Auto**. Try **Light** if the game itself is running slowly. Genesis uses Arduino SPI in v1.0.2 after a reset was reported with DMA enabled. The firmware ignores the old Genesis DMA preference. The GBA Transfer option is still available under **Game Boy Advance**.

Genesis exchanges two PSRAM framebuffers between the emulator and display task, avoiding a full-frame copy. Use Serial Monitor at **115200 baud** to compare **emulated FPS** with the roughly 60 Hz NTSC or 50 Hz PAL target, and **display FPS** to see how often a picture reaches the panel. Performance varies by game.

## Measured GBA performance

Historical **Crash Bandicoot XS** capture on ESP32-S3 at 240 MHz with 40 MHz actual TFT SPI, DMA, and Auto frameskip. Each sample covers about three seconds. Settings were captured sequentially in gameplay, so these scenes vary in workload.

| Display | Sound | Samples | Emulated FPS | Display FPS | Mean core time per frame by sample | Display busy |
| --- | --- | --- | --- | --- | --- | --- |
| Full | On | 6 | 25.9–43.9 | 13.3–19.2 | 21.1–36.5 ms | 80 / 391 |
| Native | On | 5 | 36.3–40.0 | 20.0–21.8 | 22.9–25.4 ms | 0 / 316 |
| Native | Off | 6 | 33.3–38.1 | 18.6–21.2 | 24.3–28.1 ms | 0 / 369 |

```text
GBA settings: CPU 240 MHz | TFT 40 MHz | NATIVE | frameskip AUTO | sound ON | DMA
GBA: 40.0 emulated FPS | 21.8 display FPS | core 22.9 ms/frame | draw 26.7 ms | skip 18.2 ms | display busy 0/67
```

### Read the performance log

**Emulated FPS** counts completed core frames. **Display FPS** counts completed transfers. **Draw** and **skip** are mean core times for rendered and unrendered frames. **Display busy** counts rendered frames rejected by the presenter out of all rendered frames. `GBA fast RAM` reports where VRAM, IWRAM, the fast-read map, and the core framebuffer were allocated.

For comparison, use the same ROM revision, save state, scene, and inputs. Record sound, display size, frameskip, transfer path, actual SPI clock, and firmware commit. The GBA scheduler targets about 59.7 Hz.

## Credits and licenses

Original Pocket Play code is copyright © 2026 Moutaz Baaj and licensed under GPL-2.0-or-later. See the [third-party notices](../THIRD_PARTY_NOTICES.md) for emulator, BIOS, library, toolchain, font, and hardware attribution.

## Release checklist

- Check the public source snapshot and release assets for local notes, saves, diagnostic logs, third-party media, proprietary BIOS files, and Doom IWADs.
- Review the exact linked firmware before distributing binaries. Preserve applicable license notices and provide matching corresponding source and build settings, including the open BIOS source.
- Check the ELF and map for retained code. The reference build retains minimp3 and no Helix code. Check the retained Arduino and ESP-IDF components, including the radio libraries.
- Record the verified hardware, board package, revision, installation steps, and known issues. [Contribution guide](../CONTRIBUTING.md)
