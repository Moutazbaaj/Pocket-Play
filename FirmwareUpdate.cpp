#include "FirmwareUpdate.h"
#include "Pins.h"

#include <Arduino.h>
#include <SD.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <strings.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>

namespace {
// The linker places this directly after esp_app_desc_t in the first image segment.
// Include wiring so hardware builds with different Pins.h values cannot cross-update.
[[gnu::used, gnu::section(".rodata_custom_desc")]]
const uint8_t boardId[] = {
  'P', 'O', 'C', 'K', 'E', 'T', 'P', 'L', 'A', 'Y', '-', 'S', '3', 1,
  uint8_t(kTftSck), uint8_t(kTftMosi), uint8_t(kTftCs), uint8_t(kTftDc),
  uint8_t(kTftReset), uint8_t(kTftBacklight), kTftMadctl, uint8_t(kFlashlightRgbPin),
  uint8_t(kSdSck), uint8_t(kSdMosi), uint8_t(kSdMiso), uint8_t(kSdCs),
  uint8_t(kUp), uint8_t(kDown), uint8_t(kLeft), uint8_t(kRight), uint8_t(kAnalogDpad),
  uint8_t(kA), uint8_t(kB), uint8_t(kSelect), uint8_t(kStart), uint8_t(kMenu),
  uint8_t(kL), uint8_t(kR), uint8_t(kBatterySense),
  uint8_t(kAudioBclk), uint8_t(kAudioLrclk), uint8_t(kAudioData),
};
constexpr size_t kAppOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
constexpr size_t kBoardOffset = kAppOffset + sizeof(esp_app_desc_t);
constexpr size_t kPrefixBytes = kBoardOffset + sizeof(boardId);
constexpr size_t kSdBufferBytes = 4096;

const esp_partition_t *targetPartition() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!running || !target || target == running || target->size != FirmwareUpdate::maxImageBytes ||
      running->size != FirmwareUpdate::maxImageBytes ||
      !esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr)) {
    return nullptr;
  }
  return target;
}

bool matchesBoard(const uint8_t *prefix, size_t length) {
  esp_image_header_t image;
  esp_image_segment_header_t segment;
  uint32_t appMagic;
  memcpy(&image, prefix, sizeof(image));
  memcpy(&segment, prefix + sizeof(image), sizeof(segment));
  memcpy(&appMagic, prefix + kAppOffset, sizeof(appMagic));
  if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32S3 ||
      image.segment_count == 0 || image.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
      image.hash_appended != 1 || appMagic != ESP_APP_DESC_MAGIC_WORD ||
      segment.data_len < sizeof(esp_app_desc_t) + sizeof(boardId) ||
      segment.data_len > length - kAppOffset) {
    return false;
  }
  const volatile uint8_t *installed = boardId;  // Keep the embedded descriptor in the linked image.
  for (size_t i = 0; i < sizeof(boardId); ++i) {
    if (prefix[kBoardOffset + i] != installed[i]) return false;
  }
  return true;
}
}

bool FirmwareUpdate::supportedName(const char *name) {
  constexpr char suffix[] = ".ino.bin";
  const size_t length = strlen(name);
  constexpr size_t suffixLength = sizeof(suffix) - 1;
  return length > suffixLength && strcasecmp(name + length - suffixLength, suffix) == 0;
}

size_t FirmwareUpdate::capacity() {
  const esp_partition_t *target = targetPartition();
  return target ? target->size : 0;
}

FirmwareUpdate::Result FirmwareUpdate::install(size_t length, uint8_t *buffer, size_t bufferSize,
                                               Receive receive, void *context, Progress progress) {
  const esp_partition_t *target = targetPartition();
  if (!target || bufferSize < kPrefixBytes) return Result::Unavailable;
  if (length > target->size) return Result::TooLarge;
  if (length < kPrefixBytes) return Result::WrongImage;
  if (!receive(context, buffer, kPrefixBytes)) return Result::Interrupted;
  if (!matchesBoard(buffer, length)) return Result::WrongImage;

  esp_ota_handle_t handle;
  if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) return Result::FlashError;
  esp_err_t error = esp_ota_write(handle, buffer, kPrefixBytes);
  int reportedPercent = 0;
  for (size_t written = kPrefixBytes; written < length && error == ESP_OK;) {
    const size_t count = std::min(bufferSize, length - written);
    if (!receive(context, buffer, count)) {
      esp_ota_abort(handle);
      return Result::Interrupted;
    }
    error = esp_ota_write(handle, buffer, count);
    if (error != ESP_OK) break;
    written += count;
    const int percent = std::min(int(written * 100 / length), 99);
    if (percent != reportedPercent) {
      progress(percent);
      reportedPercent = percent;
    }
  }
  if (error != ESP_OK) {
    esp_ota_abort(handle);
    return Result::FlashError;
  }
  error = esp_ota_end(handle);
  if (error == ESP_ERR_OTA_VALIDATE_FAILED) return Result::WrongImage;
  if (error != ESP_OK) return Result::FlashError;
  const esp_partition_pos_t position{target->address, target->size};
  esp_image_metadata_t metadata{};
  if (esp_image_get_metadata(&position, &metadata) != ESP_OK || metadata.image_len != length) {
    return Result::WrongImage;
  }
  if (esp_ota_set_boot_partition(target) != ESP_OK) return Result::FlashError;
  progress(100);
  return Result::Ready;
}

FirmwareUpdate::Result FirmwareUpdate::installFromSd(const char *path, Progress progress) {
  if (!supportedName(path)) return Result::WrongImage;
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) return Result::Interrupted;
  const auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kSdBufferBytes]);
  if (!buffer) return Result::Unavailable;
  return install(file.size(), buffer.get(), kSdBufferBytes,
                 [](void *context, uint8_t *destination, size_t count) {
                   auto &input = *static_cast<File *>(context);
                   const bool read = input.read(destination, count) == count;
                   if (read) delay(1);
                   return read;
                 }, &file, progress);
}

#if CONFIG_APP_ROLLBACK_ENABLE
extern "C" bool verifyRollbackLater() { return true; }
#endif

void FirmwareUpdate::verifyBoot(bool healthy) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) return;

  const esp_err_t result = healthy ? esp_ota_mark_app_valid_cancel_rollback()
                                  : esp_ota_mark_app_invalid_rollback_and_reboot();
  if (result != ESP_OK) Serial.printf("Firmware verification: %s\n", esp_err_to_name(result));
}
