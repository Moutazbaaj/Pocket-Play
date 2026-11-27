#include "GameCover.h"

#include <SD.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <new>
#include <utility>

namespace {

using GameCover::Status;
constexpr size_t kCoverPixels = GameCover::width * GameCover::height;
constexpr uint32_t kWorkerStackBytes = 4096;

struct Job {
  String path;
  String fallback;
  std::unique_ptr<uint16_t[], decltype(&free)> thumbnail{nullptr, free};
  std::atomic<bool> cancelled{false};
  std::atomic<Status> result{Status::Loading};
};

// The main loop owns the job. The worker borrows it until it publishes a final result.
std::unique_ptr<Job> job;

// Read BMP fields byte by byte because the file header is not naturally aligned.
uint16_t readLe16(const uint8_t *bytes) {
  return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8;
}

uint32_t readLe32(const uint8_t *bytes) {
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
         uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

// Decode a 24-bit BMP into a centered RGB565 thumbnail, checking cancellation between reads.
bool decodeBmp(const String &path, Job &request) {
  if (path.isEmpty() || request.cancelled.load(std::memory_order_relaxed)) return false;
  File file = SD.open(path.c_str(), FILE_READ);
  uint8_t header[54];
  if (!file || request.cancelled.load(std::memory_order_relaxed) ||
      file.read(header, sizeof(header)) != sizeof(header) || header[0] != 'B' || header[1] != 'M' ||
      readLe32(header + 14) < 40 || readLe16(header + 26) != 1 ||
      readLe16(header + 28) != 24 || readLe32(header + 30) != 0) {
    return false;
  }

  const uint32_t pixelOffset = readLe32(header + 10);
  const uint32_t dibSize = readLe32(header + 14);
  if (pixelOffset < 14 || dibSize > pixelOffset - 14) return false;
  const int32_t sourceWidth = static_cast<int32_t>(readLe32(header + 18));
  const int32_t signedHeight = static_cast<int32_t>(readLe32(header + 22));
  if (sourceWidth <= 0 || sourceWidth > 1024 || signedHeight == 0 ||
      signedHeight < -1024 || signedHeight > 1024) return false;
  const int sourceHeight = signedHeight < 0 ? -signedHeight : signedHeight;
  const uint32_t sourceRowBytes = (uint32_t(sourceWidth) * 3 + 3) & ~uint32_t(3);
  const uint64_t bitmapEnd = uint64_t(pixelOffset) + uint64_t(sourceRowBytes) * sourceHeight;
  if (bitmapEnd > file.size() || bitmapEnd > UINT32_MAX) return false;

  int drawW = GameCover::width;
  int drawH = sourceHeight * drawW / sourceWidth;
  if (drawH > GameCover::height) {
    drawH = GameCover::height;
    drawW = sourceWidth * drawH / sourceHeight;
  }
  if (drawW <= 0 || drawH <= 0 || request.cancelled.load(std::memory_order_relaxed)) return false;

  std::unique_ptr<uint8_t[]> sourceRow(new (std::nothrow) uint8_t[sourceRowBytes]);
  if (!sourceRow) return false;
  if (!request.thumbnail) {
    auto *pixels = static_cast<uint16_t *>(
        heap_caps_malloc(kCoverPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) pixels = static_cast<uint16_t *>(malloc(kCoverPixels * sizeof(uint16_t)));
    if (!pixels) return false;
    request.thumbnail.reset(pixels);
  }
  std::fill_n(request.thumbnail.get(), kCoverPixels, 0);
  const int left = (GameCover::width - drawW) / 2;
  const int top = (GameCover::height - drawH) / 2;
  const bool topDown = signedHeight < 0;
  int previousRow = -1;
  const uint16_t *previousPixels = nullptr;
  for (int outputRow = 0; outputRow < drawH; ++outputRow) {
    if (request.cancelled.load(std::memory_order_relaxed)) return false;

    // Traverse the file forward even when BMP rows are stored bottom-up.
    const int y = topDown ? outputRow : drawH - outputRow - 1;
    const int sourceY = y * sourceHeight / drawH;
    const int row = topDown ? sourceY : sourceHeight - sourceY - 1;
    auto *pixels = request.thumbnail.get() + (top + y) * GameCover::width + left;
    if (row == previousRow) {
      std::copy_n(previousPixels, drawW, pixels);
    } else {
      const uint32_t offset = pixelOffset + uint32_t(row) * sourceRowBytes;
      if (!file.seek(offset) || request.cancelled.load(std::memory_order_relaxed) ||
          file.read(sourceRow.get(), sourceRowBytes) != sourceRowBytes) {
        return false;
      }
      for (int x = 0; x < drawW; ++x) {
        const uint8_t *pixel = sourceRow.get() + (x * sourceWidth / drawW) * 3;
        pixels[x] = uint16_t(pixel[2] & 0xF8) << 8 |
                    uint16_t(pixel[1] & 0xFC) << 3 |
                    uint16_t(pixel[0]) >> 3;
      }
      previousRow = row;
      previousPixels = pixels;
    }
    // Leave time for the background audio task during a long decode.
    if ((outputRow + 1) % 8 == 0) vTaskDelay(1);
  }
  return true;
}

// Look up and decode the image off the input loop. Keep TFT access on the main loop.
void loadGameCover(void *argument) {
  auto &request = *static_cast<Job *>(argument);
  const bool loaded = decodeBmp(request.path, request) || decodeBmp(request.fallback, request);
  // Both attempts have closed their File handles. The main loop may reclaim the job after this.
  request.result.store(loaded ? Status::Ready : Status::Missing, std::memory_order_release);
  vTaskDelete(nullptr);
}

}

bool GameCover::start(String path, String fallback) {
  if (status() != Status::Idle) return false;
  job.reset(new (std::nothrow) Job);
  if (!job) return false;
  job->path = std::move(path);
  job->fallback = std::move(fallback);
  const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 - xPortGetCoreID() : 0;
  if (xTaskCreatePinnedToCore(loadGameCover, "game-cover", kWorkerStackBytes,
                             job.get(), tskIDLE_PRIORITY + 1, nullptr, core) != pdPASS) {
    job.reset();
    return false;
  }
  return true;
}

void GameCover::cancel() {
  if (!job) return;
  job->cancelled.store(true, std::memory_order_relaxed);
  status();
}

void GameCover::stop() {
  cancel();
  while (status() == Status::Loading) vTaskDelay(1);
}

GameCover::Status GameCover::status() {
  if (!job) return Status::Idle;
  const Status result = job->result.load(std::memory_order_acquire);
  if (result != Status::Loading && job->cancelled.load(std::memory_order_relaxed)) {
    job.reset();
    return Status::Idle;
  }
  return result;
}

const uint16_t *GameCover::pixels() {
  return status() == Status::Ready ? job->thumbnail.get() : nullptr;
}
