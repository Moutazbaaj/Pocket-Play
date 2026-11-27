/* mdcore - a Sega Mega Drive / Genesis emulator core.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_COMMON_H
#define MDCORE_COMMON_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#include <string.h>

/* Keep the Genesis chip symbols separate from the linked SMS Plus core. */
#define vdp_reset mdcore_vdp_reset
#define z80_init mdcore_z80_init
#define sound_reset mdcore_sound_reset
#define psg_write mdcore_psg_write

#endif
