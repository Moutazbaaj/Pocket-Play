#include "../cartridge/CartridgeCore.h"
#include "shared.h"

static const size_t kBatteryBytes = 0x8000;
static const uint32_t kMasterSystemState = 2;
static const uint32_t kGameGearState = 3;
static uint16_t palette[256];
static uint32_t savedBattery;
static uint32_t stateKind;
static bool initialized;

static void closeSega(void) {
  if (initialized) system_shutdown();
  initialized = false;
  free(bitmap.data);
  free(cart.rom);
  free(cart.sram);
  memset(&bitmap, 0, sizeof(bitmap));
  memset(&cart, 0, sizeof(cart));
  memset(&sms, 0, sizeof(sms));
  memset(&vdp, 0, sizeof(vdp));
  memset(&snd, 0, sizeof(snd));
  memset(&input, 0, sizeof(input));
}

// Accept raw images and the common 512-byte copier header.
static const char *readRom(const char *path, size_t *size, size_t *padded) {
  FILE *file = fopen(path, "rb");
  if (!file) return "Could not open Sega ROM";
  const long length = cartridgeFileSize(file);
  if (length < 8192 || length > 2 * 1024 * 1024 + 512) {
    fclose(file);
    return "Invalid Sega ROM size";
  }
  const size_t skip = length % 8192 == 512 ? 512 : 0;
  *size = length - skip;
  if (*size % 8192 != 0 || fseek(file, (long)skip, SEEK_SET) != 0) {
    fclose(file);
    return "Invalid Sega ROM size";
  }
  *padded = (*size + 0x3fff) & ~(size_t)0x3fff;
  cart.rom = cartridgeAlloc(*padded);
  if (!cart.rom) {
    fclose(file);
    return "Enable OPI PSRAM for Sega";
  }
  const bool read = fread(cart.rom, 1, *size, file) == *size;
  fclose(file);
  if (*size == 8192) {
    memcpy(cart.rom + *size, cart.rom, *size);
  } else {
    memset(cart.rom + *size, 0xff, *padded - *size);
  }
  return read ? NULL : "Could not read Sega ROM";
}

static uint32_t batteryCrc(void) {
  return cartridgeCrc32(0, cart.sram, kBatteryBytes);
}

static void batteryCommitted(void) { savedBattery = batteryCrc(); }

static const char *openSega(const char *path, const char *savePath, bool gameGear, unsigned audioRate) {
  size_t size, padded;
  const char *error = readRom(path, &size, &padded);
  if (error) goto failed;
  system_reset_config();
  option.sndrate = audioRate;
  option.overscan = 0;
  if (!load_rom(cart.rom, (int)padded, (int)size)) goto noMemory;
  if (gameGear && !IS_GG) sms.console = CONSOLE_GG;
  if (!gameGear && !IS_SMS) sms.console = CONSOLE_SMS2;
  stateKind = gameGear ? kGameGearState : kMasterSystemState;
  bitmap.width = bitmap.pitch = SMS_WIDTH;
  bitmap.height = 240;  // Space for extended modes as well as the usual 192/144 lines.
  bitmap.data = malloc(bitmap.pitch * bitmap.height);
  if (!bitmap.data) bitmap.data = cartridgeAlloc(bitmap.pitch * bitmap.height);
  if (!bitmap.data) goto noMemory;
  initialized = true;
  if (!system_poweron()) goto noMemory;

  if (cartridgeReadBattery(savePath, cart.sram, kBatteryBytes)) sms.save = 1;
  batteryCommitted();
  return NULL;

noMemory:
  error = "Insufficient RAM for Sega";
failed:
  closeSega();
  return error;
}

static unsigned refreshRate(void) { return snd.fps; }

static void runSega(uint16_t keys, bool draw) {
  static const uint8_t mapping[] = {
    INPUT_RIGHT, INPUT_LEFT, INPUT_UP, INPUT_DOWN, INPUT_BUTTON2, INPUT_BUTTON1,
  };
  input.pad[0] = 0;
  input.pad[1] = 0;
  for (unsigned i = 0; i < sizeof(mapping); ++i) {
    if (keys & (1u << i)) input.pad[0] |= mapping[i];
  }
  input.system = keys & 0x80 ? (IS_GG ? INPUT_START : INPUT_PAUSE) : 0;
  system_frame(!draw);
  if (draw && render_copy_palette(palette)) {
    for (unsigned i = 0; i < 256; ++i) {
      palette[i] = (uint16_t)((palette[i] << 8) | (palette[i] >> 8));
    }
  }
}

static CartridgeFrame frameSega(void) {
  return (CartridgeFrame){bitmap.data + bitmap.viewport.x, palette,
                          bitmap.viewport.w, bitmap.viewport.h, bitmap.pitch};
}

static size_t audioSega(int16_t *samples, size_t capacity) {
  size_t frames = capacity / 2;
  if (frames > (size_t)snd.sample_count) frames = snd.sample_count;
  for (size_t i = 0; i < frames; ++i) {
    samples[i * 2] = snd.stream[0][i];
    samples[i * 2 + 1] = snd.stream[1][i];
  }
  return frames * 2;
}

static bool batteryChanged(void) {
  return batteryCrc() != savedBattery;
}

static bool writeBattery(const char *path) {
  return cartridgeWriteBattery(path, cart.sram, kBatteryBytes);
}

static bool writeState(const char *path) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  const bool written = system_save_state(file) == 0 && !ferror(file);
  return fclose(file) == 0 && written && cartridgeSealState(path, stateKind, cart.crc);
}

static bool readState(const char *path) {
  if (!cartridgeCheckState(path, stateKind, cart.crc)) return false;
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const bool loaded = system_load_state(file);
  return fclose(file) == 0 && loaded;
}

static const char *openMasterSystem(const char *path, const char *savePath, unsigned audioRate) {
  return openSega(path, savePath, false, audioRate);
}

static const char *openGameGear(const char *path, const char *savePath, unsigned audioRate) {
  return openSega(path, savePath, true, audioRate);
}

const CartridgeCore pocketMasterSystem = {
  openMasterSystem, closeSega, refreshRate, runSega, frameSega, audioSega,
  batteryChanged, writeBattery, batteryCommitted, writeState, readState,
};

const CartridgeCore pocketGameGear = {
  openGameGear, closeSega, refreshRate, runSega, frameSega, audioSega,
  batteryChanged, writeBattery, batteryCommitted, writeState, readState,
};
