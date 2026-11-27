#include "PhotoImage.h"

#include <SD.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <iterator>
#include <strings.h>

namespace {
// Choose PSRAM for decoded image allocations when the board has it.
uint32_t imageMemoryCaps() {
  return MALLOC_CAP_8BIT | (psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
}
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_PSD
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 4096
#define STBI_MALLOC(size) heap_caps_malloc(size, imageMemoryCaps())
#define STBI_REALLOC(ptr, size) heap_caps_realloc(ptr, size, imageMemoryCaps())
#define STBI_FREE(ptr) heap_caps_free(ptr)
#include "src/stb/stb_image.h"

namespace {
struct ImageReader {
  File &file;
  const std::atomic<bool> &cancelled;

  bool stopped() const { return cancelled.load(std::memory_order_relaxed); }
};

// Stop pulling SD data when the browser no longer needs this image.
int readImage(void *context, char *out, int size) {
  auto &reader = *static_cast<ImageReader *>(context);
  if (reader.stopped()) return 0;
  return std::max(0, int(reader.file.read(reinterpret_cast<uint8_t *>(out), size)));
}

// Move the decoder's Arduino File cursor by a relative byte offset.
void skipImageBytes(void *context, int offset) {
  auto &reader = *static_cast<ImageReader *>(context);
  if (reader.stopped()) return;
  const int64_t target = std::clamp<int64_t>(int64_t(reader.file.position()) + offset, 0, reader.file.size());
  reader.file.seek(uint32_t(target));
}

// Test whether the SD file cursor has reached the end of the image.
int imageEof(void *context) {
  auto &reader = *static_cast<ImageReader *>(context);
  return reader.stopped() || reader.file.position() >= reader.file.size();
}

const stbi_io_callbacks kImageIO = {readImage, skipImageBytes, imageEof};
}

// Recognize the filename extensions the Photos browser can open.
bool PhotoImage::supportedName(const char *name) {
  const char *extension = strrchr(name, '.');
  if (!extension) return false;
  constexpr const char *supported[] = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".psd"};
  return std::any_of(std::begin(supported), std::end(supported),
                     [extension](const char *candidate) { return strcasecmp(extension, candidate) == 0; });
}

// Decode off the input loop, preserving aspect ratio and compositing alpha onto black.
PhotoImage::Result PhotoImage::load(const char *path, uint16_t *output, int width, int height,
                                    const std::atomic<bool> &cancelled) {
  if (cancelled.load(std::memory_order_relaxed)) return Result::DecodeFailed;
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) return Result::Missing;
  ImageReader reader{file, cancelled};

  const bool externalRam = psramFound();
  if (file.size() > (externalRam ? 6 * 1024 * 1024 : 256 * 1024)) return Result::TooLarge;
  const size_t maxPixels = externalRam ? 1024 * 1024 : 32768;
  int sourceWidth = 0, sourceHeight = 0;
  if (!stbi_info_from_callbacks(&kImageIO, &reader, &sourceWidth, &sourceHeight, nullptr) ||
      sourceWidth < 1 || sourceHeight < 1) {
    return Result::DecodeFailed;
  }
  if (size_t(sourceWidth) > maxPixels / size_t(sourceHeight)) return Result::TooLarge;
  if (reader.stopped() || !file.seek(0)) return Result::DecodeFailed;

  stbi_uc *rgba = stbi_load_from_callbacks(&kImageIO, &reader, &sourceWidth, &sourceHeight, nullptr, STBI_rgb_alpha);
  if (!rgba) return Result::DecodeFailed;
  file.close();

  int fitW = width, fitH = height;
  if (width * sourceHeight <= height * sourceWidth) fitH = std::max(1, width * sourceHeight / sourceWidth);
  else fitW = std::max(1, height * sourceWidth / sourceHeight);
  std::fill_n(output, width * height, uint16_t(0));
  auto *first = output + ((height - fitH) / 2) * width + (width - fitW) / 2;
  for (int y = 0; y < fitH && !reader.stopped(); ++y) {
    const auto *row = rgba + 4 * (y * sourceHeight / fitH) * sourceWidth;
    for (int x = 0; x < fitW; ++x) {
      const auto *pixel = row + 4 * (x * sourceWidth / fitW);
      const unsigned alpha = pixel[3];
      const unsigned r = pixel[0] * alpha / 255;
      const unsigned g = pixel[1] * alpha / 255;
      const unsigned b = pixel[2] * alpha / 255;
      first[y * width + x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }
  stbi_image_free(rgba);
  return reader.stopped() ? Result::DecodeFailed : Result::Drawn;
}
