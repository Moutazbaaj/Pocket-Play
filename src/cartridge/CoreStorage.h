#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Large assets go into PSRAM on the console. Heap storage also supports host tests.
void *cartridgeAlloc(size_t bytes);
uint32_t cartridgeCrc32(uint32_t crc, const uint8_t *data, size_t size);
long cartridgeFileSize(FILE *file);

bool cartridgeReadBattery(const char *path, uint8_t *data, size_t size);
bool cartridgeWriteBattery(const char *path, const uint8_t *data, size_t size);

// Bind save states to the ROM and core, and detect incomplete or altered payloads.
bool cartridgeSealState(const char *path, uint32_t core, uint32_t romCrc);
bool cartridgeCheckState(const char *path, uint32_t core, uint32_t romCrc);
