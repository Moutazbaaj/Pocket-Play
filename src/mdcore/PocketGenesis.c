#include "../cartridge/CartridgeCore.h"
#include "../cartridge/CoreStorage.h"
#include "../gnuboy/gnuboy.h"
#include "../gpsp/GbaCore.h"
#include "mdcore.h"

#include <stdlib.h>
#include <string.h>

static const uint32_t kGenesisState = 0x4D445050;
static uint8_t *rom;
static uint16_t *video;
static uint32_t romCrc;
static unsigned outputRate;
static uint32_t audioPhase;
static bool dirty;

static void closeGenesis(void) {
    md_deinit();
    free(rom);
    free(video);
    rom = NULL;
    video = NULL;
}

static const char *openGenesis(const char *romPath, const char *savePath, unsigned audioRate) {
    FILE *file = fopen(romPath, "rb");
    if (!file) return "Could not open Genesis ROM.";
    const long length = cartridgeFileSize(file);
    if (length < 0x200 || length > POCKET_GENESIS_MAX_ROM) {
        fclose(file);
        return "Invalid Genesis ROM size.";
    }

    const size_t padded = ((size_t)length + 0xFFFF) & ~(size_t)0xFFFF;
    rom = cartridgeAlloc(padded);
    video = cartridgeAlloc(MD_FB_STRIDE * MD_FB_HEIGHT * sizeof(*video));
    const bool read = rom && video && fread(rom, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!read) {
        closeGenesis();
        return "Could not load Genesis ROM.";
    }

    memset(video, 0, MD_FB_STRIDE * MD_FB_HEIGHT * sizeof(*video));
    romCrc = cartridgeCrc32(0, rom, (size_t)length);
    if (!md_init(rom, video) || !md_load((uint32_t)length, MD_REGION_AUTO)) {
        closeGenesis();
        return "Unsupported Genesis ROM.";
    }
    md_set_six_button(0, 0);
    const uint32_t saveSize = md_sram_size();
    if (saveSize) cartridgeReadBattery(savePath, md_sram(), saveSize);
    dirty = false;
    outputRate = audioRate;
    audioPhase = 0;
    return NULL;
}

static unsigned refreshRate(void) { return md_is_pal() ? 50 : 60; }
static uint32_t framePeriodUs(void) { return 1000000000u / md_fps_x1000(); }

static void runGenesis(uint16_t keys, bool draw) {
    uint32_t pad = 0;
    if (keys & GB_PAD_UP) pad |= MD_UP;
    if (keys & GB_PAD_DOWN) pad |= MD_DOWN;
    if (keys & GB_PAD_LEFT) pad |= MD_LEFT;
    if (keys & GB_PAD_RIGHT) pad |= MD_RIGHT;
    if (keys & GB_PAD_A) pad |= MD_A;
    if (keys & GB_PAD_B) pad |= MD_B;
    if (keys & (GBA_PAD_R | GB_PAD_SELECT)) pad |= MD_C;
    if (keys & GB_PAD_START) pad |= MD_START;
    md_set_input(0, pad);
    md_run_frame(draw);
}

uint16_t *pocketGenesisSwapVideoBuffer(uint16_t *buffer) {
    uint16_t *completed = video;
    video = buffer;
    md_set_framebuffer(buffer);
    return completed;
}

static CartridgeFrame frameGenesis(void) {
    return (CartridgeFrame){NULL, NULL, md_width(), md_height(), MD_FB_STRIDE, video};
}

static size_t audioGenesis(int16_t *samples, size_t capacity) {
    const int16_t *source = md_audio();
    const uint32_t rate = md_audio_rate_x1000();
    const int frames = md_audio_frames();
    size_t count = 0;
    for (int i = 0; i < frames; ++i) {
        audioPhase += outputRate * 1000;
        if (audioPhase < rate) continue;
        audioPhase -= rate;
        if (capacity - count >= 2) {
            samples[count++] = source[i * 2];
            samples[count++] = source[i * 2 + 1];
        }
    }
    return count;
}

static bool batteryChanged(void) {
    if (md_sram_dirty()) dirty = true;
    return dirty;
}

static bool writeBattery(const char *path) {
    return cartridgeWriteBattery(path, md_sram(), md_sram_size());
}

static void batteryCommitted(void) { dirty = false; }

static bool writeState(const char *path) {
    const size_t size = md_state_size();
    uint8_t *state = cartridgeAlloc(size);
    if (!state) return false;
    const bool written = md_state_save(state) == size && cartridgeWriteBattery(path, state, size);
    free(state);
    return written && cartridgeSealState(path, kGenesisState, romCrc);
}

static bool readState(const char *path) {
    if (!cartridgeCheckState(path, kGenesisState, romCrc)) return false;
    const size_t size = md_state_size();
    uint8_t *state = cartridgeAlloc(size);
    if (!state) return false;
    FILE *file = fopen(path, "rb");
    bool loaded = false;
    if (file) {
        loaded = fread(state, 1, size, file) == size && md_state_load(state, size);
        fclose(file);
    }
    free(state);
    if (loaded) dirty = md_sram_size() != 0;
    return loaded;
}

const CartridgeCore pocketGenesis = {
    openGenesis, closeGenesis, refreshRate, runGenesis, frameGenesis, audioGenesis,
    batteryChanged, writeBattery, batteryCommitted, writeState, readState, framePeriodUs,
};
