#include "GbaCore.h"
#include "common.h"
#include "bios.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_memory_utils.h>
#endif

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets;
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame;
int sprite_limit = 1;
gbsp_memory_t *gbsp_memory;
u8 *vram;
u8 *iwram;
u8 **memory_map_read;
u8 (*obj_priority_list_data)[160][128];

static uint16_t pad;
static uint8_t *savedBattery;
static bool audioEnabled = true;

static int16_t readInput(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void) port;
    (void) device;
    (void) index;
    (void) id;
    const unsigned buttons[] = {
        RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
    };
    uint16_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (pad & (1u << i))
            result |= 1u << buttons[i];
    }
    if (pad & GBA_PAD_L)
        result |= 1u << buttons[8];
    if (pad & GBA_PAD_R)
        result |= 1u << buttons[9];
    return result;
}

// The single-player Arduino frontend has no link or fast-forward callbacks.
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    (void) client_id;
    (void) buf;
    (void) len;
}
void set_fastforward_override(bool fastforward) { (void) fastforward; }

static bool writeFile(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    const bool written = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && written;
}

static long fileLength(FILE *file)
{
    if (fseek(file, 0, SEEK_END) != 0)
        return -1;
    const long size = ftell(file);
    return fseek(file, 0, SEEK_SET) == 0 ? size : -1;
}

static uint32_t stateChecksum(const uint8_t *state)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < GBA_STATE_MEM_SIZE; ++i)
        hash = (hash ^ state[i]) * 16777619u;
    return hash;
}

static long validateRom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    const long size = fileLength(file);
    uint8_t header[0xC0];
    const bool read = fread(header, 1, sizeof(header), file) == sizeof(header);
    fclose(file);
    if (!read || size < 0xC0 || size > 32L * 1024 * 1024 || header[0xB2] != 0x96)
        return -1;
    uint8_t check = 0;
    for (unsigned i = 0xA0; i <= 0xBC; ++i)
        check -= header[i];
    check -= 0x19;
    return check == header[0xBD] ? size : -1;
}

const char *gbaCoreOpen(const char *romPath, const char *biosPath, const char *savePath)
{
    gbaCoreClose();
    const long size = validateRom(romPath);
    if (size < 0)
        return "Invalid or unsupported GBA ROM";
    const char *error = "GBA needs free OPI PSRAM";
    gbsp_memory = gpsp_alloc(sizeof(*gbsp_memory));
    if (!gbsp_memory)
        goto failed;
    memset(gbsp_memory, 0, sizeof(*gbsp_memory));
    // Reserve the largest contiguous internal blocks for the renderer and
    // interpreter before allocating the framebuffer or presenter resources.
    vram = gpsp_alloc_hot(GBA_VRAM_BYTES);
    iwram = gpsp_alloc_hot(GBA_IWRAM_BYTES);
    memory_map_read = gpsp_alloc_hot(GBA_MEMORY_MAP_ENTRIES * sizeof(*memory_map_read));
    gba_screen_pixels = gpsp_alloc_framebuffer(GBA_SCREEN_BUFFER_SIZE);
    savedBattery = gpsp_alloc(sizeof(gamepak_backup));
    obj_priority_list_data = gpsp_alloc(GBA_OBJ_PRIORITY_LIST_BYTES);
    if (!vram || !iwram || !memory_map_read || !gba_screen_pixels ||
        !savedBattery || !obj_priority_list_data)
        goto failed;
    memset(gba_screen_pixels, 0, GBA_SCREEN_BUFFER_SIZE);
    memset(vram, 0, GBA_VRAM_BYTES);
    memset(iwram, 0, GBA_IWRAM_BYTES);
    memset(memory_map_read, 0, GBA_MEMORY_MAP_ENTRIES * sizeof(*memory_map_read));
    memset(obj_priority_list_data, 0, GBA_OBJ_PRIORITY_LIST_BYTES);
    if (!init_gamepak_buffer((size_t)size))
        goto failed;

    // Start with the open replacement BIOS. Accept a complete optional dump.
    _Static_assert(sizeof(open_gba_bios_rom) == sizeof(bios_rom), "Invalid BIOS size");
    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
    FILE *file = fopen(biosPath, "rb");
    if (file) {
        if (fileLength(file) != sizeof(bios_rom) ||
            fread(bios_rom, 1, sizeof(bios_rom), file) != sizeof(bios_rom))
            memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
        fclose(file);
    }

    error = "Could not read GBA ROM";
    if (load_gamepak(NULL, romPath, FEAT_AUTODETECT, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
        goto failed;

    pad = 0;
    skip_next_frame = 0;
    libretro_supports_bitmasks = true;
    retro_set_input_state(readInput);
    error = "Could not allocate GBA audio buffers";
    if (!init_sound())
        goto failed;
    sound_set_output_enabled(audioEnabled);
    reset_gba();
    update_input();

    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    file = fopen(savePath, "rb");
    if (file) {
        const long saveSize = fileLength(file);
        if ((saveSize != 512 && saveSize != 8192 && saveSize != 32768 &&
             saveSize != 65536 && saveSize != 131072) ||
            fread(gamepak_backup, 1, saveSize, file) != (size_t)saveSize) {
            fclose(file);
            error = "Could not read GBA battery save";
            goto failed;
        }
        fclose(file);
    }
    gbaCoreBatteryCommitted();
    return NULL;

failed:
    gbaCoreClose();
    return error;
}

void gbaCoreSetAudioEnabled(bool enabled)
{
    audioEnabled = enabled;
    sound_set_output_enabled(enabled);
}

void gbaCoreClose(void)
{
    memory_term();
    sound_term();
    free(iwram);
    iwram = NULL;
    free(memory_map_read);
    memory_map_read = NULL;
    free(vram);
    vram = NULL;
    free(obj_priority_list_data);
    obj_priority_list_data = NULL;
    free(gbsp_memory);
    gbsp_memory = NULL;
    free(gba_screen_pixels);
    gba_screen_pixels = NULL;
    free(savedBattery);
    savedBattery = NULL;
}

bool gbaCoreRun(uint16_t keys, bool draw)
{
    pad = keys;
    skip_next_frame = !draw;
    update_input();
    rumble_frame_reset();
    clear_gamepak_stickybits();
    execute_arm(execute_cycles);
    return !gamepak_io_failed;
}

const uint16_t *gbaCorePixels(void)
{
    return gba_screen_pixels;
}

uint8_t gbaCoreFastMemoryMask(void)
{
    uint8_t mask = 0;
#if defined(ARDUINO_ARCH_ESP32)
    if (!gbsp_memory)
        return 0;
    if (esp_ptr_internal(vram))
        mask |= GBA_FAST_MEMORY_VRAM;
    if (esp_ptr_internal(iwram))
        mask |= GBA_FAST_MEMORY_IWRAM;
    if (esp_ptr_internal(memory_map_read))
        mask |= GBA_FAST_MEMORY_MAP;
    if (esp_ptr_internal(gba_screen_pixels))
        mask |= GBA_FAST_MEMORY_FRAMEBUFFER;
#endif
    return mask;
}

size_t gbaCoreAvailableAudio(void)
{
    return audioEnabled ? sound_available_frames() * 2 : 0;
}

size_t gbaCoreReadAudio(int16_t *samples, size_t capacity)
{
    if (!audioEnabled)
        return 0;
    return sound_read_samples(samples, capacity / 2) * 2;
}

bool gbaCoreBatteryChanged(void)
{
    return memcmp(gamepak_backup, savedBattery, sizeof(gamepak_backup)) != 0;
}

bool gbaCoreWriteBattery(const char *filename)
{
    // gpSP/libretro uses one fixed-size save image for SRAM, Flash and EEPROM.
    return writeFile(filename, gamepak_backup, sizeof(gamepak_backup));
}

void gbaCoreBatteryCommitted(void)
{
    memcpy(savedBattery, gamepak_backup, sizeof(gamepak_backup));
}

bool gbaCoreWriteState(const char *filename)
{
    const size_t size = GBA_STATE_MEM_SIZE + sizeof(uint32_t);
    uint8_t *state = gpsp_alloc(size);
    if (!state)
        return false;
    memset(state, 0, GBA_STATE_MEM_SIZE);
    gba_save_state(state);
    uint8_t *checksum = state + GBA_STATE_MEM_SIZE;
    bson_write_u32(checksum, stateChecksum(state));
    const bool saved = writeFile(filename, state, size);
    free(state);
    return saved;
}

bool gbaCoreReadState(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
        return false;
    const size_t size = GBA_STATE_MEM_SIZE + sizeof(uint32_t);
    uint8_t *state = gpsp_alloc(size);
    bool loaded = false;
    if (state && fileLength(file) == (long)size && fread(state, 1, size, file) == size) {
        const uint8_t *checksum = state + GBA_STATE_MEM_SIZE;
        loaded = stateChecksum(state) == bson_read_u32(checksum) && gba_load_state(state);
    }
    fclose(file);
    free(state);
    return loaded;
}
