#pragma once

#include <Arduino.h>

#include "src/cartridge/CartridgeCore.h"

// Draw directly to the shared ILI9341 in landscape. The caller must serialize screen operations.
namespace Screen {
constexpr int width = 320;
constexpr int height = 240;
constexpr int marqueeGapPixels = 24;
constexpr int compactTextAdvance = 6;
constexpr uint16_t black = 0x0000;
constexpr uint16_t white = 0xFFFF;
struct ThemeColors {
  uint16_t background, header, panel, selected, highlight, text, muted, accent;
};
constexpr ThemeColors darkTheme = {0x10A3, 0x0861, 0x18E3, 0x2945, 0x2530, white, 0xAD75, 0x07F5};
extern const ThemeColors &theme;
void setLightTheme(bool light);
bool lightTheme();
// Adjust a console or app accent for legibility on the light palette.
uint16_t themeAccent(uint16_t color);

enum class GbScale : uint8_t { Native, Fit, Full };

// Return true if the screen bus and panel initialized.
bool begin();
// Release the Arduino bus. Returns false if unavailable or owned by GBA DMA.
bool suspendSpi();
// Restore the Arduino bus after temporarily borrowing its pins.
bool resumeSpi();
// Turn off the panel and backlight. Optionally wait for the panel to enter sleep.
bool sleepPanel(bool waitForTransition = false);
// Schedule panel wake, including any needed SPI restoration.
void wakePanel();
// Poll on each loop until the panel is ready for drawing.
bool finishPanelWake();
// Accept a percentage from 0 to 100. Return whether the backlight is controllable and the request succeeded.
bool setBrightness(uint8_t percent);
// Set the requested TFT clock in hertz. The GBA DMA transport caps its own clock.
void setClockHz(uint32_t clockHz);
// Read the configured clock in hertz.
uint32_t clockHz();
// Fill a pixel rectangle with an RGB565 color.
void fill(int x, int y, int w, int h, uint16_t color);
// Draw text using the 8 by 8 font, an explicit background, and an integer scale.
void text(int x, int y, const char *value, uint16_t fg, uint16_t bg, int scale = 1);
enum class TextFont { Standard, Compact };
// Draw a centered label using the 5 by 7 font.
void compactLabel(int centerX, int y, const char *value, uint16_t fg, uint16_t bg);
// Draw a one-row viewport of width w with a horizontal pixel offset.
void marqueeText(int x, int y, int w, const char *value, int offset, uint16_t fg, uint16_t bg,
                 TextFont font = TextFont::Standard);
// Transfer a contiguous RGB565 rectangle to the screen.
void drawRgb565(const uint16_t *pixels, int x, int y, int w, int h);
// Draw 160 by 144 RGB565 output from the GB and GBC core.
void drawGameFrame(const void *pixels, GbScale scale);
// Draw a palette-indexed cartridge frame. The adapter supplies a validated viewport.
void drawCartridgeFrame(const CartridgeFrame &frame, GbScale scale);
// Acquire the TFT bus for GBA DMA. Return true when DMA is active.
bool beginGbaVideo();
// Release GBA DMA and restore menu SPI.
void endGbaVideo();
// Draw a 240 by 160 RGB565 frame and return whether the transfer succeeded.
bool drawGbaFrame(const uint16_t *pixels, bool fullScreen);
}
