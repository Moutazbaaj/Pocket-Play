#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_PRINTF, NULL, x)
#else
#define LOG_PRINTF(level, x...) printf(x)
#define IRAM_ATTR
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
// #define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) {}

#define GB_WIDTH (160)
#define GB_HEIGHT (144)

typedef uint8_t byte;

typedef enum
{
	GB_HW_DMG,
	GB_HW_CGB,
	GB_HW_SGB,
} gb_hwtype_t;

typedef enum
{
	GB_PAD_RIGHT = 0x01,
	GB_PAD_LEFT = 0x02,
	GB_PAD_UP = 0x04,
	GB_PAD_DOWN = 0x08,
	GB_PAD_A = 0x10,
	GB_PAD_B = 0x20,
	GB_PAD_SELECT = 0x40,
	GB_PAD_START = 0x80,
} gb_padbtn_t;

typedef enum
{
	GB_PALETTE_0,
	GB_PALETTE_1,
	GB_PALETTE_2,
	GB_PALETTE_3,
	GB_PALETTE_4,
	GB_PALETTE_5,
	GB_PALETTE_6,
	GB_PALETTE_7,
	GB_PALETTE_8,
	GB_PALETTE_9,
	GB_PALETTE_10,
	GB_PALETTE_11,
	GB_PALETTE_12,
	GB_PALETTE_13,
	GB_PALETTE_14,
	GB_PALETTE_15,
	GB_PALETTE_16,
	GB_PALETTE_17,
	GB_PALETTE_18,
	GB_PALETTE_19,
	GB_PALETTE_20,
	GB_PALETTE_21,
	GB_PALETTE_22,
	GB_PALETTE_23,
	GB_PALETTE_24,
	GB_PALETTE_25,
	GB_PALETTE_26,
	GB_PALETTE_27,
	GB_PALETTE_28,
	GB_PALETTE_29,
	GB_PALETTE_30,
	GB_PALETTE_31,
	GB_PALETTE_DMG,
	GB_PALETTE_MGB0,
	GB_PALETTE_MGB1,
	GB_PALETTE_CGB,
	GB_PALETTE_SGB,
	GB_PALETTE_COUNT,
} gb_palette_t;

typedef enum
{
	GB_PIXEL_PALETTED,
	GB_PIXEL_565_LE,
	GB_PIXEL_565_BE,
} gb_video_fmt_t;

typedef enum
{
	GB_AUDIO_STEREO_S16,
	GB_AUDIO_MONO_S16,
} gb_audio_fmt_t;

typedef void (gb_video_cb_t)(void *buffer);
typedef void (gb_audio_cb_t)(void *buffer, size_t length);

// Initialize the emulated hardware and set output formats and callbacks. Returns 0 on success.
int  gnuboy_init(int samplerate, gb_audio_fmt_t audio_fmt, gb_video_fmt_t video_fmt, gb_video_cb_t *video_callback, gb_audio_cb_t *audio_callback);
// Copy a boot ROM from memory into the core. Returns 0 on success.
int  gnuboy_load_bios(const byte *data, size_t size);
// Read the boot ROM from a stdio path and install it in the core.
int  gnuboy_load_bios_file(const char *file);
// Release the core's copy of the boot ROM.
void gnuboy_free_bios(void);
// Configure the cartridge from an in-memory ROM. Keep its bytes alive until gnuboy_free_rom().
int  gnuboy_load_rom(const byte *data, size_t size);
// Open a cartridge from a stdio path and cache ROM banks on demand. Returns 0 on success.
int  gnuboy_load_rom_file(const char *file);
// Release cartridge buffers and close the open ROM and SRAM files.
void gnuboy_free_rom(void);
// Reset the hardware, video, CPU, and sound modules using the requested reset mode.
void gnuboy_reset(bool hard);
// Advance one frame, optionally generating video. This call can invoke both output callbacks.
void gnuboy_run(bool draw);
// Check the current cartridge for pending save RAM writes.
bool gnuboy_sram_dirty(void);
// Load a cartridge bank by index into the ROM bank cache.
void gnuboy_load_bank(int);
// Set the buttons held in the current GB_PAD bitmask.
void gnuboy_set_pad(int);

// Assign a caller-owned pixel buffer in the format selected at initialization.
void gnuboy_set_framebuffer(void *buffer);
// Assign caller-owned PCM storage. Length counts individual int16_t samples.
void gnuboy_set_soundbuffer(void *buffer, size_t length);

// Read the cartridge RTC as day counter, hour, minute, and second.
void gnuboy_get_time(int *day, int *hour, int *minute, int *second);
// Set the cartridge RTC components.
void gnuboy_set_time(int day, int hour, int minute, int second);
// Read the active emulated hardware type.
int  gnuboy_get_hwtype(void);
// Reserved hardware selector. This port chooses the model when loading the cartridge.
void gnuboy_set_hwtype(gb_hwtype_t type);
// Return the active compatibility palette.
int  gnuboy_get_palette(void);
// Select the palette used to colorize monochrome games.
void gnuboy_set_palette(gb_palette_t pal);

// Load battery-backed cartridge data from a stdio path.
int gnuboy_load_sram(const char *file);
// Write cartridge save data in the requested full or quick mode.
int gnuboy_save_sram(const char *file, bool quick_save);
// Restore the active cartridge's emulator state from a stdio path.
int gnuboy_load_state(const char *file);
// Write the active cartridge's emulator state to a stdio path.
int gnuboy_save_state(const char *file);
