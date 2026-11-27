#include "AppPackage.h"
#include "AppSdk.h"

#include <SD.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char suffix[] = ".ppapp";
constexpr char magic[] = "-- PocketPlay SDK 1";
constexpr char titlePrefix[] = "-- Title: ";
constexpr char installTemp[] = "/sd/apps/.install.tmp";
constexpr char saveDirectory[] = "/app-data";
static_assert(AppSdk::version == 1);

bool idChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool readLine(File &file, char *line, size_t capacity) {
  size_t used = 0;
  for (;;) {
    const int c = file.read();
    if (c == '\n') {
      if (used && line[used - 1] == '\r') --used;
      line[used] = '\0';
      return true;
    }
    if (c == -1 || c == 0 || used + 1 >= capacity) return false;
    line[used++] = char(c);
  }
}

bool readHeader(File &file, AppPackage::Info &info) {
  if (!file || file.isDirectory() || file.size() > AppPackage::maxBytes) return false;
  char line[AppPackage::maxTitle + sizeof(titlePrefix) + 1];
  if (!readLine(file, line, sizeof(line)) || strcmp(line, magic) != 0 ||
      !readLine(file, line, sizeof(line)) || strncmp(line, titlePrefix, sizeof(titlePrefix) - 1) != 0) {
    return false;
  }
  const char *title = line + sizeof(titlePrefix) - 1;
  const size_t length = strlen(title);
  if (!length || length > AppPackage::maxTitle || file.position() >= file.size()) return false;
  for (const char *c = title; *c; ++c) {
    if (*c < ' ' || *c > '~') return false;
  }
  if (title[0] == ' ' || title[length - 1] == ' ') return false;
  memcpy(info.title, title, length + 1);
  return true;
}

bool exists(const char *path) {
  struct stat info;
  return stat(path, &info) == 0;
}

bool ensureDirectory(const char *path) {
  if (!SD.exists(path) && !SD.mkdir(path)) return false;
  File directory = SD.open(path, FILE_READ);
  return directory && directory.isDirectory();
}

bool copyPackage(File &source, const char *destination) {
  const int file = open(installTemp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (file < 0) return false;
  bool complete = source.seek(0);
  uint8_t bytes[512];
  size_t remaining = source.size();
  while (complete && remaining) {
    const size_t count = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    complete = source.read(bytes, count) == count && write(file, bytes, count) == ssize_t(count);
    remaining -= count;
  }
  const bool synced = complete && fsync(file) == 0;
  const bool closed = close(file) == 0;
  if (synced && closed && rename(installTemp, destination) == 0) return true;
  unlink(installTemp);
  return false;
}

void dataPath(char *path, size_t size, const char *id, const char *extension) {
  snprintf(path, size, "/sd%s/%s.%s", saveDirectory, id, extension);
}
}

bool AppPackage::supportedName(const char *name) {
  const size_t length = strlen(name);
  if (length <= sizeof(suffix) - 1 || length > maxId + sizeof(suffix) - 1 || !idChar(*name)) return false;
  const size_t base = length - (sizeof(suffix) - 1);
  if (strcmp(name + base, suffix)) return false;
  for (size_t i = 1; i < base; ++i) {
    if (!idChar(name[i]) && name[i] != '-' && name[i] != '_') return false;
  }
  return true;
}

bool AppPackage::inspect(const char *path, Info &info) {
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  if (!supportedName(name)) return false;
  const size_t length = strlen(name) - (sizeof(suffix) - 1);
  memcpy(info.id, name, length);
  info.id[length] = '\0';
  File file = SD.open(path, FILE_READ);
  return readHeader(file, info);
}

AppPackage::Result AppPackage::install(const char *source, const char *name) {
  Info info;
  if (!supportedName(name)) return Result::Invalid;
  File file = SD.open(source, FILE_READ);
  if (!readHeader(file, info)) return Result::Invalid;
  if (!ensureDirectory(directory)) return Result::StorageError;
  char destination[sizeof("/sd/apps/") + maxId + sizeof(suffix)];
  snprintf(destination, sizeof(destination), "/sd%s/%s", directory, name);
  if (exists(destination)) return Result::Exists;
  return copyPackage(file, destination) ? Result::Installed : Result::StorageError;
}

bool AppPackage::validHeader(const char *path) {
  Info info;
  File file = SD.open(path, FILE_READ);
  return readHeader(file, info);
}

bool AppPackage::save(const char *id, const char *data, size_t length) {
  if (length > maxSave || !ensureDirectory(saveDirectory)) return false;
  char path[64], temp[64], backup[64];
  dataPath(path, sizeof(path), id, "sav");
  dataPath(temp, sizeof(temp), id, "tmp");
  dataPath(backup, sizeof(backup), id, "bak");
  const int file = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (file < 0) return false;
  const bool synced = write(file, data, length) == ssize_t(length) && fsync(file) == 0;
  const bool closed = close(file) == 0;
  if (!synced || !closed || (exists(path) && (unlink(backup), rename(path, backup) != 0))) {
    unlink(temp);
    return false;
  }
  if (rename(temp, path) != 0) {
    rename(backup, path);
    unlink(temp);
    return false;
  }
  unlink(backup);
  return true;
}

int AppPackage::load(const char *id, char *data) {
  char path[64];
  dataPath(path, sizeof(path), id, "sav");
  FILE *file = fopen(path, "rb");
  if (!file) {
    dataPath(path, sizeof(path), id, "bak");
    file = fopen(path, "rb");
  }
  if (!file) return -1;
  const size_t length = fread(data, 1, maxSave, file);
  const bool complete = fgetc(file) == EOF && !ferror(file);
  return fclose(file) == 0 && complete ? int(length) : -1;
}
