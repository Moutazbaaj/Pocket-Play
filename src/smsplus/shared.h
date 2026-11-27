#ifndef _SHARED_H_
#define _SHARED_H_

#include <stdint.h>

typedef unsigned char uint8;
typedef unsigned short int uint16;
typedef uint32_t uint32;

typedef signed char int8;
typedef signed short int int16;
typedef int32_t int32;

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>

#include "../cartridge/CoreStorage.h"

#define LOG_PRINTF(level, x...) printf(x)
#define crc32_le(a, b, c) cartridgeCrc32(a, b, c)
#ifdef ARDUINO_ARCH_ESP32
#include <esp_attr.h>
#pragma GCC optimize ("O3")
#else
#define IRAM_ATTR
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)

#define sound_init smsplus_sound_init
#define sound_reset smsplus_sound_reset
#define sound_shutdown smsplus_sound_shutdown
#define sound_update smsplus_sound_update
#include "cpu/SegaCpu.h"
#include "memz80.h"
#include "loadrom.h"
#include "pio.h"
#include "render.h"
#include "sms.h"
#include "state.h"
#include "system.h"
#include "tms.h"
#include "vdp.h"
#include "sound/emu76489.h"
#include "sound/sound.h"

#endif /* _SHARED_H_ */
