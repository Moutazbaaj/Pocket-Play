#pragma once

#include <stddef.h>
#include <stdint.h>

namespace FirmwareUpdate {
inline constexpr char directory[] = "/updates";
inline constexpr size_t maxImageBytes = 3 * 1024 * 1024;
enum class Result { Ready, Unavailable, TooLarge, WrongImage, Interrupted, FlashError };
using Receive = bool (*)(void *context, uint8_t *buffer, size_t count);
using Progress = void (*)(int percent);

bool supportedName(const char *name);
size_t capacity();
// Report whole percentages as flash writes succeed. 100 means the new boot slot is ready.
Result install(size_t length, uint8_t *buffer, size_t bufferSize, Receive receive, void *context,
               Progress progress);
Result installFromSd(const char *path, Progress progress);
void verifyBoot(bool healthy);
}
