#pragma once

#include <stdint.h>

namespace SnakeGame {
void reset(uint32_t now);
void draw();
void update(uint32_t now, uint16_t pressed);
}
