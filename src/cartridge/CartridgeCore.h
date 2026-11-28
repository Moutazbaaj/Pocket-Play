#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const uint8_t *pixels;
  const uint16_t *palette;
  int width;
  int height;
  int pitch;
  const uint16_t *rgb565;  // Optional native pixels in place of pixels and palette.
} CartridgeFrame;

// Single active cartridge. ROM and save paths use the SD VFS prefix.
// Frame and audio output belongs to the core until its next run().
typedef struct {
  const char *(*open)(const char *romPath, const char *savePath, unsigned audioRate);
  void (*close)(void);
  unsigned (*refreshRate)(void);
  void (*run)(uint16_t keys, bool draw);
  CartridgeFrame (*frame)(void);
  size_t (*audio)(int16_t *samples, size_t capacity);
  bool (*batteryChanged)(void);
  bool (*writeBattery)(const char *path);
  void (*batteryCommitted)(void);
  bool (*writeState)(const char *path);
  bool (*readState)(const char *path);
  uint32_t (*framePeriodUs)(void);  // Optional fractional refresh rate.
} CartridgeCore;

extern const CartridgeCore pocketNes;
extern const CartridgeCore pocketMasterSystem;
extern const CartridgeCore pocketGameGear;
extern const CartridgeCore pocketGenesis;
// Exchange the Genesis render buffer for a free 320 by 240 RGB565 buffer.
uint16_t *pocketGenesisSwapVideoBuffer(uint16_t *buffer);
#define POCKET_GENESIS_MAX_ROM (6u * 1024u * 1024u)

#ifdef __cplusplus
}
#endif
