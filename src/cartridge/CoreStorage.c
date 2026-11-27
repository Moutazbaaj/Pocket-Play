#include "CoreStorage.h"

#include <stdlib.h>

#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#endif

void *cartridgeAlloc(size_t bytes) {
#ifdef ARDUINO_ARCH_ESP32
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

uint32_t cartridgeCrc32(uint32_t crc, const uint8_t *data, size_t size) {
#ifdef ARDUINO_ARCH_ESP32
  return esp_rom_crc32_le(crc, data, size);
#else
  crc = ~crc;
  while (size--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
#endif
}

long cartridgeFileSize(FILE *file) {
  if (fseek(file, 0, SEEK_END) != 0) return -1;
  const long size = ftell(file);
  return fseek(file, 0, SEEK_SET) == 0 ? size : -1;
}

bool cartridgeReadBattery(const char *path, uint8_t *data, size_t size) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const bool read = cartridgeFileSize(file) == (long)size && fread(data, 1, size, file) == size;
  return fclose(file) == 0 && read;
}

bool cartridgeWriteBattery(const char *path, const uint8_t *data, size_t size) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  const bool written = fwrite(data, 1, size, file) == size;
  return fclose(file) == 0 && written;
}

static const uint32_t kStateMagic = 0x31535050;  // PPS1
static const unsigned kStateMaxBytes = 512 * 1024;
typedef struct {
  uint32_t magic, core, romCrc, size, dataCrc;
} StateTrailer;

// CRC the payload in bounded chunks so save handling needs little internal RAM.
static bool stateCrc(FILE *file, size_t size, uint32_t *crc) {
  uint8_t data[512];
  *crc = 0;
  while (size) {
    const size_t count = size < sizeof(data) ? size : sizeof(data);
    if (fread(data, 1, count, file) != count) return false;
    *crc = cartridgeCrc32(*crc, data, count);
    size -= count;
  }
  return true;
}

bool cartridgeSealState(const char *path, uint32_t core, uint32_t romCrc) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const long size = cartridgeFileSize(file);
  StateTrailer trailer = {kStateMagic, core, romCrc, 0, 0};
  const bool valid = size > 0 && size <= kStateMaxBytes && stateCrc(file, size, &trailer.dataCrc);
  fclose(file);
  if (!valid) return false;
  trailer.size = (uint32_t)size;
  file = fopen(path, "ab");
  if (!file) return false;
  const bool written = fwrite(&trailer, sizeof(trailer), 1, file) == 1;
  return fclose(file) == 0 && written;
}

bool cartridgeCheckState(const char *path, uint32_t core, uint32_t romCrc) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const long size = cartridgeFileSize(file) - (long)sizeof(StateTrailer);
  uint32_t crc = 0;
  StateTrailer trailer;
  const bool valid = size > 0 && size <= kStateMaxBytes && stateCrc(file, size, &crc) &&
                     fread(&trailer, sizeof(trailer), 1, file) == 1 &&
                     trailer.magic == kStateMagic && trailer.core == core &&
                     trailer.romCrc == romCrc && trailer.size == (uint32_t)size &&
                     trailer.dataCrc == crc;
  fclose(file);
  return valid;
}
