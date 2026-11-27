#include "AppSdk.h"
#include "Display.h"
#include <algorithm>

static_assert(AppSdk::width == Screen::width && AppSdk::height < Screen::height);

void AppSdk::fill(int x, int y, int w, int h, uint16_t color) {
  const int left = std::clamp(x, 0, width);
  const int top = std::clamp(y, 0, height);
  const int right = std::clamp<int64_t>(int64_t(x) + w, left, width);
  const int bottom = std::clamp<int64_t>(int64_t(y) + h, top, height);
  Screen::fill(left, top, right - left, bottom - top, color);
}

void AppSdk::text(int x, int y, const char *label, uint16_t fg, uint16_t bg, int scale) {
  if (scale >= 1 && scale <= 3 && x >= 0 && x < width && y >= 0 && y <= height - 8 * scale) {
    Screen::text(x, y, label, fg, bg, scale);
  }
}

void AppSdk::sprite(int x, int y, int w, int h, const uint16_t *pixels) {
  if (pixels && w > 0 && h > 0 && x >= 0 && x <= width - w && y >= 0 && y <= height - h) {
    Screen::drawRgb565(pixels, x, y, w, h);
  }
}
