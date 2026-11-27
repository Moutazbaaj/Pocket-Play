#pragma once

#include "../../cartridge/CoreStorage.h"

#define LOG_PRINTF(level, x...) printf(x)
#define CRC32(a, b, c) cartridgeCrc32(a, b, c)
#ifdef ARDUINO_ARCH_ESP32
#include <esp_attr.h>
#pragma GCC optimize ("O3")
#else
#define IRAM_ATTR
#endif

#define MESSAGE_ERROR(x...) LOG_PRINTF(1, "!! " x)
#define MESSAGE_WARN(x...)  LOG_PRINTF(2, " ! " x)
#define MESSAGE_INFO(x...)  LOG_PRINTF(3, x)
#ifdef NOFRENDO_DEBUG
#define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, "> %s: " x, __func__, ## __VA_ARGS__)
#else
#define MESSAGE_DEBUG(x...)
#endif
#define MESSAGE_TRACE(x, ...) LOG_PRINTF(4, "~ %s: " x, __func__, ## __VA_ARGS__)

#undef MIN
#define MIN(a, b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a < _b ? _a : _b; })
#undef MAX
#define MAX(a, b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a > _b ? _a : _b; })

#define ASSERT(expr) while (!(expr)) { LOG_PRINTF(1, "ASSERTION FAILED IN %s: " #expr "\n", __func__); abort(); }
#define UNUSED(x) (void)x
