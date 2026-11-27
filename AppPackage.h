#pragma once

#include <stddef.h>

namespace AppPackage {
inline constexpr char directory[] = "/apps";
inline constexpr char inbox[] = "/app-install";
constexpr size_t maxBytes = 16 * 1024;
constexpr size_t maxId = 32;
constexpr size_t maxTitle = 32;
constexpr size_t maxSave = 256;

struct Info {
  char id[maxId + 1]{};
  char title[maxTitle + 1]{};
};

enum class Result { Installed, Invalid, Exists, StorageError };

bool supportedName(const char *name);
bool inspect(const char *path, Info &info);
bool validHeader(const char *path);
// source is relative to microSD. name is the destination filename.
Result install(const char *source, const char *name);
// Each app owns one small record named after its package ID.
bool save(const char *id, const char *data, size_t length);
int load(const char *id, char *data);
}
