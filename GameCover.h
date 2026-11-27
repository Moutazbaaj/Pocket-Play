#pragma once

#include <Arduino.h>

namespace GameCover {

constexpr int width = 86;
constexpr int height = 112;
enum class Status : uint8_t { Idle, Loading, Ready, Missing };

// Schedule one background lookup. The fallback is tried if the first BMP cannot be read.
bool start(String path, String fallback);

// Discard the result immediately. An in-flight SD call may finish on the worker.
void cancel();

// Cancel and wait for the SD file to close before a remount or sleep.
void stop();

// Poll on the main loop. A cancelled job stays Loading until the worker has released its file.
Status status();

// Borrow the completed RGB565 image until cancel() or stop().
const uint16_t *pixels();

}
