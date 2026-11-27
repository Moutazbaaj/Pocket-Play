#pragma once

#include <stdlib.h>

// Retain Retro-Go's dynamically allocated memory layout.
#define RETRO_GO 1
#define ROM_BUFFER_SIZE 6

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#include <esp_heap_caps.h>
#define GPSP_HOT_FUNCTION IRAM_ATTR
#else
#define GPSP_HOT_FUNCTION
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O3", "no-strict-aliasing")
#endif

static inline void *gpsp_alloc(size_t size)
{
#if defined(ARDUINO_ARCH_ESP32)
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

// Interpreter hot data benefits greatly from internal SRAM, but using PSRAM
// as a fallback keeps the core usable on memory-constrained configurations.
static inline void *gpsp_alloc_hot(size_t size)
{
#if defined(ARDUINO_ARCH_ESP32)
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr ? ptr : gpsp_alloc(size);
#else
    return malloc(size);
#endif
}

// The display reads the finished GBA frame every refresh. Prefer internal,
// DMA-capable SRAM for it; fall back to PSRAM rather than refusing to start
// on boards whose internal heap is too fragmented.
static inline void *gpsp_alloc_framebuffer(size_t size)
{
#if defined(ARDUINO_ARCH_ESP32)
    void *ptr = heap_caps_malloc(size,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    return ptr ? ptr : gpsp_alloc(size);
#else
    return malloc(size);
#endif
}
