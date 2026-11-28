# Pocket Play hardware

The reference build uses an ESP32-S3-WROOM-1-N16R8, a 320 × 240 ILI9341 TFT, an SPI microSD reader, and momentary buttons. The MAX98357A amplifier, speaker, and battery circuit are optional. [Firmware setup](../BUILDING.md#build-and-upload)

## Prototype photos and dimensions

The original prototype is built on a **150 × 90 mm (15 × 9 cm)** perfboard. The photos show where the major components sit on that build. Use the pin tables below for electrical connections.

| View | What it shows |
| --- | --- |
| [Front](images/prototype/front.jpg) | TFT, buttons, step-up converter, USB-C charger, and SD card. |
| [Rear](images/prototype/rear.jpg) | ESP32-S3 development board, battery, and the two speakers. |
| [Button edge](images/prototype/button-edge.jpg) | Relative heights of the buttons, display, battery, and speakers. |
| [Charger side](images/prototype/charger-side.jpg) | USB-C charger and power switch. |
| [Card edge](images/prototype/card-edge.jpg) | SD card access under the display. |
| [Amplifier supply close-up](images/prototype/amplifier-supply-capacitor.jpg) · [With both capacitors fitted](images/prototype/amplifier-supply-capacitors.jpg) | Underside of the perfboard during audio power testing. |

[Bottom shell measurements](#bottom-enclosure-prototype)

## Display, storage, and audio

The TFT and microSD reader use separate SPI buses. Use 3.3 V logic, connect the grounds, and follow each breakout’s supply requirements. [Default pin assignments](../Pins.h)

| Module | Signal | GPIO |
| --- | --- | --- |
| ILI9341 | SCK, MOSI, CS, DC, RESET | 48, 45, 21, 47, 3 |
| ILI9341 | Backlight enable | 38 |
| microSD | SCK, MOSI, MISO, CS | 12, 10, 13, 11 |
| MAX98357A | BCLK, LRCLK, DIN | 40, 2, 41 |
| Onboard RGB | Data | 48 |

Leave TFT MISO and touch pins disconnected. GPIO38 supplies an active-high 3.3 V control signal. If the TFT backlight pad takes power directly, wire GPIO38 to the enable pin of a backlight switch. The onboard RGB LED shares GPIO48 with the TFT clock on the reference build. [ESP32-S3 DevKitC-1 pinout](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)

## Buttons and serial controls

Wire each button between its GPIO and ground. Set either shoulder pin to `-1` in `Pins.h` to omit that button. For the resistor-ladder D-pad, set `kAnalogDpad = true` and use GPIO6 and GPIO7.

| Input | GPIO | Serial |
| --- | --- | --- |
| Up / Down | 6 / 7 | W / S |
| Left / Right | 9 / 14 | A / D |
| A / B | 15 / 5 | J / K |
| Select / Start | 16 / 17 | U / I |
| Menu | 18 | M |
| L / R | 1 / 4 | Q / E |
| Rescan microSD | | R |

Use the USB-to-UART port at 115200 baud and select **No line ending** for one-letter commands.

## Battery and 5 V power

The reference circuit uses one 3.7 V Li-ion cell rated for a 4.2 V charge, an LC2H00530A0 protected USB-C charger module, and an SX1308 boost converter. Verify that the charger’s current matches the cell rating.

```text
Cell + ── charger B+          Charger OUT+ ── power switch ── SX1308 IN+
Cell − ── charger B−          Charger OUT− ────────────────── SX1308 IN−

SX1308 OUT+ (5 V) ──┬── dev board 5V input
                    └── amplifier VIN
SX1308 OUT− (GND) ──┬── dev board GND
                    └── amplifier GND
```

Disconnect the cell and USB before wiring. Keep the charger’s battery and load terminals separate. With the dev board and amplifier disconnected, adjust the converter to 5.0 V DC and verify it with a multimeter. Power off before attaching the load. Charge through the charger module with the console switched off. Disconnect the external 5 V feed before powering the dev board over USB. [DevKitC-1 power options](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html#power-supply-options) · [TP4056 datasheet](https://www.toppwr.com/uploadfile/file/20230304/640301eae1260.pdf) · [SX1308 datasheet](https://atta.szlcsc.com/upload/public/pdf/source/20160920/1474356679185.pdf)

### Amplifier power and display flicker

On the reference build, moving the MAX98357A supply from the ESP32’s 3.3 V rail to the regulated 5 V output stopped display flicker during playback. Connect the speaker across `SPK+` and `SPK−`. Check the breakout’s existing bypass capacitors. The reference wiring uses 10 µF and 100 nF near the amplifier, with optional 100 µF bulk capacitance. Observe polarity on electrolytic capacitors. If flicker persists, compare the boost converter and ESP32 rail voltages during the same passage with playback paused and active. [MAX98357A datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf)

### Battery indicator

```text
SX1308 IN+ ── 100 kΩ ──●── 100 kΩ ── GND
                       ├── GPIO8
                       └── 100 nF ── GND
```

Use the switched battery voltage at the converter input. Verify the divider midpoint is half the cell voltage before connecting it to GPIO8. Place the 100 nF capacitor near the pin. Set `kBatterySense = -1` for builds without the divider.

**Settings → Device Tools → Battery / SD** shows the averaged voltage. The percentage is estimated from cell voltage and turns red at 20% or less. The charger’s protection circuit supplies the cutoff. [ESP32-S3 ADC wiring](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html#adc)

## USB keyboard and gamepad

Use the native USB/OTG port, a data-capable host adapter, and a current-limited 5 V VBUS supply. Follow the board’s USB host power design or use a self-powered USB 2.0 hub with upstream backfeed protection. Use a single supply for the console. Flash and monitor through the separate USB-to-UART port. [USB host hardware guide](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html)

| Input | USB boot keyboard | Standard USB HID gamepad |
| --- | --- | --- |
| D-pad | Arrow keys or W / A / S / D | Hat or X / Y axes |
| A / B | Z / X, J / K, or Enter / Backspace | HID buttons 1 / 2 |
| Start / Select | I / U | HID buttons 10 / 9 |
| L / R | Q / E | HID buttons 5 / 6 |
| Menu | Escape or M | HID button 11 or Start + Select |

Check mapping in **Settings → Device Tools → Button Test**. Physical Start and Select are required for light-sleep wake. Set `POCKETPLAY_USB_CONTROLS` to `0` for a buttons-only build.

## Bluetooth controllers

Pocket Play supports one Bluetooth LE HID gamepad at a time, including Xbox Wireless Controllers with compatible BLE firmware. The radio starts disabled. After the first pairing, Pocket Play remembers the bonded controller and scans for its return while Bluetooth is running. Turning Bluetooth off preserves the bond. Sony DualShock and DualSense use Bluetooth Classic in wireless mode. [Pairing steps](../README-USER.md#pair-a-bluetooth-controller) · [Controller protocol reference](https://bluepad32.readthedocs.io/en/latest/supported_gamepads/)

### Troubleshoot Bluetooth startup

At 115200 baud, check the boot log for 8 MiB PSRAM, the Bluetooth free/largest internal heap values, and `Bluetooth: startup complete`. The radio needs at least 64 KiB free internal heap and a 40 KiB largest block. An interrupted Bluetooth startup triggers one retry on the next boot. If the retry fails, choose **Enable on Restart** and **Restart Console** to schedule another attempt. Hold the physical **Start + Select** buttons during reset to disable Bluetooth. If the failure happens on wake, save and quit the game before retrying. Include the startup log when reporting the issue. [Bluetooth startup code](../BluetoothControls.cpp)

The Arduino-ESP32 3.3.2 reference build gives the Bluetooth core’s IPC task a 3 KiB stack to cover the driver’s interrupt allocation during radio startup. [IPC source and trace details](../src/esp32_ipc/README.md)

## Bottom enclosure prototype

The prototype bottom shell fits a 150 × 90 mm PCB. It measures 170.8 × 107.8 × 29.2 mm outside, with 26 mm of clearance below the PCB. The STL is separate from the firmware. Print it flat-side down at 100% and check the openings against your assembled board.
