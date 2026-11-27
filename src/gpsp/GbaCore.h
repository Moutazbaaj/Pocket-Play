#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GBA_FRAME_WIDTH = 240,
    GBA_FRAME_HEIGHT = 160,
    GBA_PAD_RIGHT = 1 << 0,
    GBA_PAD_LEFT = 1 << 1,
    GBA_PAD_UP = 1 << 2,
    GBA_PAD_DOWN = 1 << 3,
    GBA_PAD_A = 1 << 4,
    GBA_PAD_B = 1 << 5,
    GBA_PAD_SELECT = 1 << 6,
    GBA_PAD_START = 1 << 7,
    GBA_PAD_L = 1 << 9,
    GBA_PAD_R = 1 << 10,
};

// Disable output before opening a game to trade GBA sound for performance.
void gbaCoreSetAudioEnabled(bool enabled);
// Open one game at a time using stdio paths. Returns NULL on success or a launcher error message.
const char *gbaCoreOpen(const char *romPath, const char *biosPath, const char *savePath);
// Release the session's ROM handle, cache, and emulator buffers.
void gbaCoreClose(void);
// Run one emulated frame with the given button mask. Returns false on ROM I/O failure.
bool gbaCoreRun(uint16_t keys, bool draw);
// Borrow the current 240 by 160 RGB565 frame. Copy it before another frame overwrites it.
const uint16_t *gbaCorePixels(void);
enum {
    GBA_FAST_MEMORY_VRAM = 1 << 0,
    GBA_FAST_MEMORY_IWRAM = 1 << 1,
    GBA_FAST_MEMORY_MAP = 1 << 2,
    GBA_FAST_MEMORY_FRAMEBUFFER = 1 << 3,
};
// Report which hot buffers are in internal RAM using the GBA_FAST_MEMORY flags.
uint8_t gbaCoreFastMemoryMask(void);
// Counts individual interleaved int16_t values (two per stereo frame).
size_t gbaCoreAvailableAudio(void);
// Drain complete stereo frames. Capacity and return value count individual int16_t values.
size_t gbaCoreReadAudio(int16_t *samples, size_t capacity);
// Compare cartridge backup memory with the last committed copy.
bool gbaCoreBatteryChanged(void);
// Write the complete backup image to the supplied stdio path.
bool gbaCoreWriteBattery(const char *filename);
// Update the saved baseline after the caller has published the backup file.
void gbaCoreBatteryCommitted(void);
// Write a save state with its checksum to the supplied stdio path.
bool gbaCoreWriteState(const char *filename);
// Check file size and checksum before attempting to restore the active game's state.
bool gbaCoreReadState(const char *filename);

#ifdef __cplusplus
}
#endif
