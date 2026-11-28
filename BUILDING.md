# Build and flash Pocket Play

[Project overview](README.md) · [Player guide](README-USER.md) · [Hardware and wiring](docs/HARDWARE.md)

## Build and upload

Clone the repository into an Arduino sketch folder named `PocketPlay`. If you downloaded `PocketPlay-source.zip` from a release, extract the included `PocketPlay` folder. For a GitHub-generated source ZIP, rename the extracted folder to `PocketPlay`.

```sh
git clone https://github.com/Moutazbaaj/Pocket-Play.git PocketPlay
```

1. Open `PocketPlay.ino` in Arduino IDE. Keep the sketch, its other source files, and the `src/` directory together.
2. In **Boards Manager**, select **esp32 by Espressif Systems**, choose **3.3.2** in the version list, and install it. This is the reference build and Wi-Fi Transfer version on macOS and Windows. The board package version is independent of the Arduino IDE version. If the package is missing, add Espressif’s stable package index under **Additional Boards Manager URLs** in Preferences. [Installation guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) · [Arduino-ESP32 3.3.2 release](https://github.com/espressif/arduino-esp32/releases/tag/3.3.2)
3. Select the board options below. Connect Pocket Play through the **USB-to-UART** port and select that serial port.
4. Click **Verify**, then **Upload**. Close any other serial monitor if the port is busy. If the board fails to enter download mode automatically, hold **BOOT**, tap **RESET/EN**, then release **BOOT** and retry the upload. [Espressif upload troubleshooting](https://docs.espressif.com/projects/arduino-esp32/en/latest/troubleshooting.html)
5. Open Serial Monitor at **115200 baud**. The startup log reports the detected flash and PSRAM. The target module should report **16 MiB flash** and **8 MiB PSRAM**. On the handheld, open **Settings → About → About Pocket Play → Build version** to confirm the installed firmware and Arduino-ESP32 versions.

| Arduino IDE option | Value for the N16R8 build |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| Flash Mode | QIO 80MHz |
| PSRAM | OPI PSRAM |
| CPU Frequency | 240MHz (WiFi) |
| Arduino Runs On | Core 1 |
| Events Run On | Core 1 |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Disabled |
| USB DFU On Boot | Disabled |
| USB Firmware MSC On Boot | Disabled |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 115200 |
| Erase All Flash Before Sketch Upload | Disabled |
| Core Debug Level | None |
| JTAG Adapter | Disabled |
| Zigbee Mode | Disabled |

Use **OPI PSRAM** for the N16R8 module. The emulators use external RAM for ROM data and other large allocations. Games, music, and saves live on removable microSD. The reference partition scheme defines two **3 MiB** firmware slots in onboard flash. It also reserves an internal FAT partition separate from the microSD card. [Pinned 3.3.2 partition table](https://raw.githubusercontent.com/espressif/arduino-esp32/3.3.2/tools/partitions/app3M_fat9M_16MB.csv)

### If the sketch exceeds the app partition

If Verify reports `Sketch too big` or `text section exceeds available space in board` with a maximum of **1,310,720 bytes**, select **Tools → Partition Scheme → 16M Flash (3MB APP/9.9MB FATFS)** with **Flash Size → 16MB (128Mb)**. This gives the sketch **3,145,728 bytes**. Verify again, then upload over the USB-to-UART port so the new partition table is written. [Espressif partition table guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/partition_table.html)

### Package firmware for USB and Wi-Fi

Select the reference settings above, then choose **Sketch → Export Compiled Binary**. Arduino IDE 2 saves the files under `PocketPlay/build/esp32.esp32.esp32s3/`. Keep the bootloader, partition table, and app from the same export.

| File | Use |
| --- | --- |
| `PocketPlay.ino.bin` | App image for Wi-Fi OTA and microSD updates. Also used at `0x10000` in the split USB package. |
| `PocketPlay.ino.bootloader.bin` | USB bootloader image at `0x0`. |
| `PocketPlay.ino.partitions.bin` | USB partition table image at `0x8000`. |
| `boot_app0.bin` | USB initial OTA data at `0xe000`. Copy it from `tools/partitions/boot_app0.bin` inside the installed Arduino-ESP32 3.3.2 package. |
| `PocketPlay.ino.merged.bin` | Optional complete USB image at `0x0` for blank boards or an intentional full replacement. The 16 MB export is padded and can overwrite saved settings and other onboard data. |

For a first USB install, a partition change, or recovery, use Arduino IDE **Upload** or flash the four split files. Close Serial Monitor, plug into the USB-to-UART port, and replace `YOUR_SERIAL_PORT` with the actual port. [Espressif esptool commands](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html)

```sh
esptool --chip esp32s3 --port YOUR_SERIAL_PORT --baud 115200 write-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 PocketPlay.ino.bootloader.bin 0x8000 PocketPlay.ino.partitions.bin \
  0xe000 boot_app0.bin 0x10000 PocketPlay.ino.bin
```

Release the app image and USB package as clearly labeled downloads. The app may be renamed to include a version or Git revision if it still ends in `.ino.bin`. Include SHA-256 checksums, the exact source revision and build settings, corresponding source, and license notices. [Third-party notices](THIRD_PARTY_NOTICES.md)

### Troubleshoot Wi-Fi and firmware updates

| What you see | What to check |
| --- | --- |
| The hotspot is missing, the client has no IPv4 address, or the page fails to load | Confirm Pocket Play is on the **Wi-Fi Transfer** screen, join the shown network, and use the exact HTTP address. If this started after compiling or flashing from another computer, confirm **esp32 by Espressif Systems 3.3.2** and the full reference board settings there, then rebuild and upload over USB. |
| The page says the console needs a USB firmware update, or **Update Software** is missing | Install a current build over USB with the reference 16 MB dual-slot partition scheme. An OTA firmware update leaves that partition table in place. |
| The updater reports an invalid image, different wiring, or a size error | Use the matching Pocket Play app file ending in `.ino.bin`, check its release checksum, and confirm it fits the 3 MiB slot. |
| The upload is interrupted | Reconnect and retry. A new transfer session clears any unfinished temporary microSD upload. For repeated resets, capture the `Wi-Fi upload` lines and the next boot’s `Reset reason` at 115200 baud. |
| Pocket Play cannot boot after installing | Use the [USB install or recovery package](#package-firmware-for-usb-and-wi-fi). |
