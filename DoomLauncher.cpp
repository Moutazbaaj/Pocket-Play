#include "DoomLauncher.h"
#include "Display.h"
#include "Pins.h"
#include "src/doom/DoomSound.h"
#include "src/doom/doomgeneric.h"
#include "src/doom/doomgeneric_fs.h"
#include "src/doom/doomkeys.h"
#include "src/doom/palette_esp32.h"
#include "src/gpsp/GbaCore.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <SD.h>
#include <algorithm>
#include <iterator>

extern "C" {
#include "src/gnuboy/gnuboy.h"
}

namespace {
uint16_t palette[256]{};
DoomLauncher::ReadControls readControls = nullptr;
uint16_t menuMask = 0;
uint16_t stableKeys = 0, rawKeys = 0, pendingKeys = 0;
uint32_t changedAt = 0;
char iwadPath[32];
char saveDirectory[48];

class DoomAudio final : public IDoomPCMOutput {
 public:
  uint8_t volume = 0;

  bool begin(uint32_t rate) override {
    output.setPins(kAudioBclk, kAudioLrclk, kAudioData);
    return output.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  }

  void write(const int16_t *samples, size_t count) override {
    int16_t stereo[256];
    while (count) {
      const size_t frames = std::min(count, std::size(stereo) / 2);
      for (size_t i = 0; i < frames; ++i) {
        const int16_t sample = int32_t(samples[i]) * volume / 100;
        stereo[2 * i] = stereo[2 * i + 1] = sample;
      }
      output.write(reinterpret_cast<uint8_t *>(stereo), frames * 2 * sizeof(int16_t));
      count -= frames;
      samples += frames;
    }
  }

  void end() override { output.end(); }

 private:
  I2SClass output;
};
DoomAudio audio;

uint32_t littleEndian(const uint8_t *bytes) {
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
         uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

bool validIwad(File &file) {
  uint8_t header[12];
  if (!file || file.isDirectory() || file.size() < sizeof(header) ||
      file.size() > DoomLauncher::maxWadBytes || file.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "IWAD", 4) != 0) return false;
  const uint32_t count = littleEndian(header + 4);
  const uint32_t offset = littleEndian(header + 8);
  if (!count || count > 16384 || offset < sizeof(header) || offset > file.size() ||
      count > (file.size() - offset) / 16 || !file.seek(offset)) return false;

  bool paletteFound = false, firstMapFound = false;
  uint8_t entries[256];
  for (uint32_t done = 0; done < count;) {
    const uint32_t batch = std::min<uint32_t>(count - done, sizeof(entries) / 16);
    if (file.read(entries, 16 * batch) != 16 * batch) return false;
    for (uint32_t i = 0; i < batch; ++i) {
      const uint8_t *entry = entries + 16 * i;
      const uint32_t start = littleEndian(entry);
      const uint32_t length = littleEndian(entry + 4);
      if (length && (start > file.size() || length > file.size() - start)) return false;
      if (memcmp(entry + 8, "PLAYPAL", 8) == 0 && length >= 768) paletteFound = true;
      if (memcmp(entry + 8, "E1M1\0\0\0", 8) == 0 ||
          memcmp(entry + 8, "MAP01\0\0", 8) == 0) firstMapFound = true;
    }
    done += batch;
    delay(1);
  }
  return paletteFound && firstMapFound;
}
}

bool DoomLauncher::supportedName(const char *name) {
  for (const char *file : files) {
    if (!strcasecmp(name, file)) return true;
  }
  return false;
}

bool DoomLauncher::available(unsigned index) {
  if (index >= std::size(files)) return false;
  char path[32];
  snprintf(path, sizeof(path), "%s/%s", directory, files[index]);
  return SD.exists(path);
}

const char *DoomLauncher::start(unsigned index, ReadControls controls, uint16_t menu, bool sound, uint8_t volume) {
  if (index >= std::size(files)) return "Choose a DOOM IWAD.";
  if (ESP.getPsramSize() < 8u * 1024u * 1024u) return "DOOM needs 8 MiB PSRAM.";
  snprintf(iwadPath, sizeof(iwadPath), "%s/%s", directory, files[index]);
  {
    File file = SD.open(iwadPath);
    if (!validIwad(file)) return "Missing or invalid DOOM IWAD.";
  }
  if (!SD.exists("/doom/saves") && !SD.mkdir("/doom/saves")) return "Could not create DOOM saves.";
  char directoryOnCard[44];
  snprintf(directoryOnCard, sizeof(directoryOnCard), "/doom/saves/%s", files[index]);
  if (!SD.exists(directoryOnCard) && !SD.mkdir(directoryOnCard)) return "Could not open DOOM saves.";
  snprintf(saveDirectory, sizeof(saveDirectory), "/sd%s", directoryOnCard);

  DG_FS = &SD;
  readControls = controls;
  menuMask = menu;
  audio.volume = volume;
  DG_PCM = sound && volume ? &audio : nullptr;
  static char program[] = "doom";
  static char iwad[] = "-iwad";
  static char config[] = "-savedir";
  static char noMusic[] = "-nomusic";
  static char noSound[] = "-nosound";
  static char *argv[] = {program, iwad, iwadPath, config, saveDirectory, noMusic, noSound, nullptr};
  Screen::fill(0, 0, Screen::width, Screen::height, Screen::black);
  doomgeneric_Create(DG_PCM ? 6 : 7, argv);
  return nullptr;
}

void DoomLauncher::tick() {
  doomgeneric_Tick();
  delay(1);
}

extern "C" void DG_Init() {}
extern "C" void DG_SetWindowTitle(const char *) {}
extern "C" void DG_SleepMs(uint32_t ms) { delay(ms ? ms : 1); }
extern "C" uint32_t DG_GetTicksMs() { return millis(); }

extern "C" void DG_PaletteLoad(const void *playpal) {
  const auto *rgb = static_cast<const uint8_t *>(playpal);
  for (size_t i = 0; i < std::size(palette); ++i, rgb += 3) {
    palette[i] = uint16_t((rgb[0] & 0xF8) << 8) | uint16_t((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
  }
}

extern "C" void DG_DrawFrame() {
  Screen::drawCartridgeFrame({DG_ScreenBuffer, palette, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                              DOOMGENERIC_RESX, nullptr}, Screen::GbScale::Full);
}

extern "C" int DG_GetKey(int *pressed, unsigned char *key) {
  if (!pendingKeys) {
    const uint16_t held = readControls();
    if (held != rawKeys) {
      rawKeys = held;
      changedAt = millis();
    }
    if (millis() - changedAt >= 12) {
      pendingKeys = stableKeys ^ rawKeys;
      stableKeys = rawKeys;
    }
  }
  const struct { uint16_t mask; unsigned char key; } mapping[] = {
    {GB_PAD_UP, KEY_UPARROW}, {GB_PAD_DOWN, KEY_DOWNARROW},
    {GB_PAD_LEFT, KEY_LEFTARROW}, {GB_PAD_RIGHT, KEY_RIGHTARROW},
    {GB_PAD_A, KEY_FIRE}, {GB_PAD_B, KEY_USE}, {GB_PAD_START, KEY_ENTER},
    {GB_PAD_SELECT, KEY_TAB}, {GBA_PAD_L, KEY_STRAFE_L}, {GBA_PAD_R, KEY_STRAFE_R},
    {menuMask, KEY_ESCAPE},
  };
  for (const auto &button : mapping) {
    if (!(pendingKeys & button.mask)) continue;
    pendingKeys &= ~button.mask;
    *pressed = (stableKeys & button.mask) != 0;
    *key = button.key;
    return 1;
  }
  pendingKeys = 0;
  return 0;
}

extern "C" [[noreturn]] void DG_Exit(const char *error) {
  if (error) {
    Serial.printf("DOOM: %s\n", error);
    Screen::fill(0, 0, Screen::width, Screen::height, Screen::black);
    Screen::text(12, 24, "DOOM COULD NOT CONTINUE", Screen::darkTheme.text, Screen::black);
    Screen::text(12, 48, "CHECK THE IWAD AND SERIAL LOG", Screen::darkTheme.muted, Screen::black);
    Screen::text(12, 200, "A / B: RESTART", Screen::darkTheme.text, Screen::black);
    constexpr uint16_t buttons = GB_PAD_A | GB_PAD_B;
    while (readControls() & buttons) delay(15);
    while (!(readControls() & buttons)) delay(15);
  }
  ESP.restart();
  for (;;) delay(100);
}
