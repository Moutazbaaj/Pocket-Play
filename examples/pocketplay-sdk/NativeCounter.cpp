#include "NativeCounter.h"

#include <stdio.h>

// Call these functions from a firmware view. Keep the system loop in PocketPlay.ino.
namespace NativeCounter {
namespace {
constexpr uint16_t background = AppSdk::rgb(14, 24, 30);
constexpr uint16_t foreground = AppSdk::rgb(209, 250, 231);
unsigned count = 0;
AppSdk::PlayTone tone = nullptr;

void drawCount() {
  char label[32];
  snprintf(label, sizeof(label), "Count %u", count);
  AppSdk::fill(16, 68, 288, 16, background);
  AppSdk::text(16, 68, label, foreground, background, 2);
}
}

void draw() {
  AppSdk::fill(0, 0, AppSdk::width, AppSdk::height, background);
  AppSdk::text(16, 26, "Native Counter", foreground, background, 2);
  drawCount();
}

void open(AppSdk::PlayTone playTone) {
  count = 0;
  tone = playTone;
  draw();
}

void update(AppSdk::Input input) {
  if (input.pressed & AppSdk::B) count = 0;
  else if (input.pressed & AppSdk::A) {
    ++count;
    if (tone) tone(650, 50);
  } else {
    return;
  }
  drawCount();
}

void close() { tone = nullptr; }
}
