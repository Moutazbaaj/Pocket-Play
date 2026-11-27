#include "../cartridge/CartridgeCore.h"
#include "../cartridge/CoreStorage.h"
#include "nofrendo.h"
#include "nes/nes.h"

// Launcher adapters use the same eight input bits as the GB and GBA paths.
static nes_t *nes;
static uint8_t *romData;
static uint8_t *video;
static uint16_t *palette;
static uint32_t savedBattery;
static const uint32_t kNesState = 1;

static void closeNes(void) {
  if (nes) nes_shutdown();
  nes = NULL;
  free(palette);
  palette = NULL;
  free(video);
  video = NULL;
  free(romData);
  romData = NULL;
}

// Validate iNES sizes before the core can inspect the trainer or calculate its checksum.
static const char *readRom(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (!file) return "Could not open NES ROM";
  const long length = cartridgeFileSize(file);
  uint8_t header[16];
  const bool valid = length >= (long)sizeof(header) && length <= 4 * 1024 * 1024 &&
                     fread(header, sizeof(header), 1, file) == 1 &&
                     memcmp(header, "NES\x1a", 4) == 0 && header[4] != 0;
  if (!valid) {
    fclose(file);
    return "Invalid iNES ROM";
  }
  if ((header[7] & 0x0C) == 8) {
    fclose(file);
    return "NES 2.0 format unsupported";
  }
  const size_t gameBytes = sizeof(header) + (header[6] & 4 ? 512 : 0) +
                           header[4] * 16384u + header[5] * 8192u;
  if (gameBytes > (size_t)length) {
    fclose(file);
    return "Truncated NES ROM";
  }

  romData = cartridgeAlloc(gameBytes);
  if (!romData) {
    fclose(file);
    return "Enable OPI PSRAM for NES";
  }
  memcpy(romData, header, sizeof(header));
  const bool read = fread(romData + sizeof(header), 1, gameBytes - sizeof(header), file) ==
                    gameBytes - sizeof(header);
  fclose(file);
  *size = gameBytes;
  return read ? NULL : "Could not read NES ROM";
}

static size_t batterySize(void) {
  return nes->cart->prg_ram_banks * ROM_PRG_BANK_SIZE;
}

static uint32_t batteryCrc(void) {
  return cartridgeCrc32(0, nes->cart->prg_ram, batterySize());
}

static void batteryCommitted(void) {
  savedBattery = batteryCrc();
}

static const char *openNes(const char *path, const char *savePath, unsigned audioRate) {
  size_t size = 0;
  const char *error = readRom(path, &size);
  if (error) goto failed;
  video = malloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
  if (!video) video = cartridgeAlloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
  palette = nofrendo_buildpalette(NES_PALETTE_PVM, 16);
  nes = nes_init(SYS_DETECT, audioRate, true, NULL);
  if (!video || !palette || !nes) {
    error = "Insufficient RAM for NES";
    goto failed;
  }
  rom_t *rom = rom_loadmem(romData, size);
  if (rom && rom->system == SYS_UNKNOWN &&
      (strstr(path, "(Europe)") || strstr(path, "(E)") ||
       strstr(path, "(Australia)") || strstr(path, "(A)"))) {
    rom->system = SYS_NES_PAL;
  }
  const int result = nes_insertcart(rom);
  if (result != 0) {
    // Nofrendo releases its internal state when insertion fails.
    nes = NULL;
    error = result == -2 ? "Unsupported NES mapper" : "Could not initialize NES ROM";
    goto failed;
  }
  if (nes->cart->battery) {
    cartridgeReadBattery(savePath, nes->cart->prg_ram, batterySize());
    batteryCommitted();
  }
  // Bring the PPU through the initial vblank before restoring a snapshot.
  nes_emulate(false);
  nes_emulate(false);
  return NULL;

failed:
  closeNes();
  return error;
}

static unsigned refreshRate(void) { return nes->refresh_rate; }

static void runNes(uint16_t keys, bool draw) {
  // Reverse the handheld's direction and action groups into NES pad bit order.
  static const uint8_t mapping[] = {
    NES_PAD_RIGHT, NES_PAD_LEFT, NES_PAD_UP, NES_PAD_DOWN,
    NES_PAD_A, NES_PAD_B, NES_PAD_SELECT, NES_PAD_START,
  };
  uint8_t pad = 0;
  for (unsigned i = 0; i < sizeof(mapping); ++i) {
    if (keys & (1u << i)) pad |= mapping[i];
  }
  input_update(0, pad);
  if (draw) nes_setvidbuf(video);
  nes_emulate(draw);
}

static CartridgeFrame frameNes(void) {
  const int crop = nes->overscan;
  return (CartridgeFrame){NES_SCREEN_GETPTR(video, 0, crop), palette,
                          NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT - crop * 2, NES_SCREEN_PITCH};
}

static size_t audioNes(int16_t *samples, size_t capacity) {
  const size_t available = nes->apu->samples_per_frame * 2;
  const size_t count = available < capacity ? available : capacity & ~(size_t)1;
  memcpy(samples, nes->apu->buffer, count * sizeof(*samples));
  return count;
}

static bool batteryChanged(void) {
  return nes->cart->battery && batteryCrc() != savedBattery;
}

static bool writeBattery(const char *path) {
  return nes->cart->battery && cartridgeWriteBattery(path, nes->cart->prg_ram, batterySize());
}

static bool writeState(const char *path) {
  return state_save(path) == 0 && cartridgeSealState(path, kNesState, nes->cart->checksum);
}

static bool readState(const char *path) {
  return cartridgeCheckState(path, kNesState, nes->cart->checksum) && state_load(path) == 0;
}

const CartridgeCore pocketNes = {
  openNes, closeNes, refreshRate, runNes, frameNes, audioNes,
  batteryChanged, writeBattery, batteryCommitted, writeState, readState,
};
