#pragma once

#include <stddef.h>
#include <stdint.h>

namespace DoomLauncher {
constexpr char directory[] = "/doom";
constexpr size_t maxWadBytes = 32u * 1024u * 1024u;
constexpr const char *files[] = {"doom1.wad", "doom.wad", "doom2.wad"};
using ReadControls = uint16_t (*)();

bool supportedName(const char *name);
bool available(unsigned index);
const char *start(unsigned index, ReadControls controls, uint16_t menuMask, bool sound, uint8_t volume);
void tick();
}
