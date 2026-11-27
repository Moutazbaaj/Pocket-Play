#pragma once

#include <stdint.h>

// Set to 0 for a buttons-only build that leaves the native USB host unlinked.
#define POCKETPLAY_USB_CONTROLS 1

// Pin assignments shared by the launcher and its device modules.

// ILI9341 on SPI2. These are the published N16R8 handheld display pins.
constexpr int kTftSck = 48;
constexpr int kTftMosi = 45;
constexpr int kTftCs = 21;
constexpr int kTftDc = 47;
constexpr int kTftReset = 3;
constexpr int kTftBacklight = 38; // Active-high BL/EN signal for a 3.3 V logic input or load switch.
constexpr uint8_t kTftMadctl = 0xA8;
constexpr uint32_t kTftClockHz = 80000000;
constexpr int kFlashlightRgbPin = 48; // Onboard RGB LED. Shares the default TFT clock pin.

// Optional FAT32 microSD module on SPI3.
constexpr int kSdSck = 12;
constexpr int kSdMosi = 10;
constexpr int kSdMiso = 13;
constexpr int kSdCs = 11;

// Optional switches between GPIO and GND. The action buttons follow the original
// layout. Four independent switches are the default for a new breadboard build.
constexpr int kUp = 6;
constexpr int kDown = 7;
constexpr int kLeft = 9;
constexpr int kRight = 14;
constexpr int kA = 15;
constexpr int kB = 5;
constexpr int kSelect = 16;
constexpr int kStart = 17;
constexpr int kMenu = 18;
constexpr int kL = 1; // Optional GBA shoulder button. Set to -1 to disable.
constexpr int kR = 4; // Optional GBA shoulder button. Set to -1 to disable.

// Set true for the published two-channel resistor-ladder D-pad on GPIO6 and 7.
constexpr bool kAnalogDpad = false;

// Midpoint of a 100 kΩ / 100 kΩ divider from switched 1-cell battery power to GND.
// Set to -1 on builds without a divider.
constexpr int kBatterySense = 8;

// Default sound state for a fresh installation. The on-device Settings menu
// controls sound afterwards and can initialize a wired I2S amplifier on demand.
constexpr bool kEnableAudio = false;
constexpr int kAudioBclk = 40;
constexpr int kAudioLrclk = 2;
constexpr int kAudioData = 41;
