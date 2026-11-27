#pragma once

#include <atomic>
#include <stdint.h>

// Size-limited image decoding into a caller-owned RGB565 buffer.
namespace PhotoImage {
enum class Result { Drawn, Missing, TooLarge, DecodeFailed };

bool supportedName(const char *name);
Result load(const char *path, uint16_t *output, int width, int height,
            const std::atomic<bool> &cancelled);
}
