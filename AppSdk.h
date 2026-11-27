#pragma once

#include <stdint.h>
#include "ControllerInput.h"

// The main loop is the only owner of the app display and input.
namespace AppSdk {
constexpr int version = 1;
constexpr int width = 320;
constexpr int height = 224;  // Pocket Play draws its exit hint below the app.
enum Button : uint16_t {
  Right = 0x01, Left = 0x02, Up = 0x04, Down = 0x08,
  A = 0x10, B = 0x20, Select = 0x40, Start = 0x80, L = 0x200, R = 0x400,
};
using Input = ControllerInput;
using PlayTone = bool (*)(unsigned frequency, unsigned durationMs);

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
void fill(int x, int y, int w, int h, uint16_t color);
void text(int x, int y, const char *label, uint16_t fg, uint16_t bg, int scale = 1);
// An RGB565 sprite must fit completely in the app display.
void sprite(int x, int y, int w, int h, const uint16_t *pixels);
}
