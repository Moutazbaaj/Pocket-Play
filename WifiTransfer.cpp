#include "WifiTransfer.h"
#include "AppPackage.h"
#include "FirmwareUpdate.h"
#include "WifiTransferPage.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <esp_http_server.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr char kNetworkKey[] = "wifiNetwork";
constexpr char kBootKey[] = "wifiNextBoot";
constexpr char kAddress[] = "http://192.168.4.1";
constexpr char kTemporaryFile[] = "/sd/.pocketplay-upload.tmp";
constexpr char kTemporarySdPath[] = "/.pocketplay-upload.tmp";
constexpr uint32_t kIdleMs = 10 * 60 * 1000;
constexpr uint32_t kReceiveIdleMs = 8000;
constexpr size_t kMaxFileName = 238;  // Leave room for save and profile suffixes.
constexpr size_t kChunkBytes = 4096;

struct NetworkCredentials {
  char name[WifiTransfer::maxNameBytes + 1]{};
  char password[WifiTransfer::maxPasswordBytes + 1]{};
};

NetworkCredentials network;
Preferences *store = nullptr;
bool networkSaved = false;
bool nextBoot = false;
bool sdAvailable = false;
bool firmwareReady = false;
httpd_handle_t server = nullptr;
WifiTransfer::ResolveDestination resolveDestination = nullptr;
char sessionToken[33]{};
enum class SessionState : uint8_t { Idle, Transfer, Closing };
std::atomic<SessionState> sessionState{SessionState::Closing};
std::atomic<uint32_t> lastActivity{0};
std::atomic<uint32_t> restartAt{0};
std::atomic<int> firmwareInstallPercent{-1};
// The HTTP server handles one request at a time.
std::unique_ptr<uint8_t[]> uploadBuffer;

class ActiveTransfer {
 public:
  ActiveTransfer() {
    auto expected = SessionState::Idle;
    active = sessionState.compare_exchange_strong(expected, SessionState::Transfer);
  }

  ~ActiveTransfer() {
    if (!active) return;
    lastActivity.store(millis());
    auto expected = SessionState::Transfer;
    sessionState.compare_exchange_strong(expected, SessionState::Idle);
  }

  explicit operator bool() const { return active; }

 private:
  bool active = false;
};

bool validText(const char *text, size_t minimum, size_t maximum) {
  const size_t length = strnlen(text, maximum + 1);
  if (length < minimum || length > maximum) return false;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < ' ' || text[i] > '~') return false;
  }
  return true;
}

bool persistNetwork(const NetworkCredentials &value) {
  if (!store || store->putBytes(kNetworkKey, &value, sizeof(value)) != sizeof(value)) return false;
  network = value;
  networkSaved = true;
  return true;
}

esp_err_t reply(httpd_req_t *request, const char *status, const char *message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_sendstr(request, message);
}

// Close the socket on a rejected request so an unread file body cannot affect the next request.
esp_err_t reject(httpd_req_t *request, const char *status, const char *message) {
  reply(request, status, message);
  return ESP_FAIL;
}

bool localHost(httpd_req_t *request) {
  char host[24];
  return httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) == ESP_OK &&
         (strcmp(host, kAddress + sizeof("http://") - 1) == 0 || strcmp(host, "192.168.4.1:80") == 0);
}

bool authorized(httpd_req_t *request) {
  char token[sizeof(sessionToken)];
  return sessionState.load() != SessionState::Closing && !restartAt.load() && localHost(request) &&
         httpd_req_get_hdr_value_str(request, "X-Pocket-Token", token, sizeof(token)) == ESP_OK &&
         strcmp(token, sessionToken) == 0;
}

esp_err_t page(httpd_req_t *request) {
  if (!localHost(request)) return reject(request, "403 Forbidden", "Open http://192.168.4.1 on Pocket Play Wi-Fi.");
  lastActivity.store(millis());
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Content-Security-Policy",
                    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                    "connect-src 'self'; frame-ancestors 'none'");
  return httpd_resp_send(request, kTransferPage, sizeof(kTransferPage) - 1);
}

esp_err_t session(httpd_req_t *request) {
  if (!localHost(request)) return reject(request, "403 Forbidden", "Use the address shown on Pocket Play.");
  lastActivity.store(millis());
  httpd_resp_set_hdr(request, "X-Pocket-SD", sdAvailable ? "ready" : "missing");
  httpd_resp_set_hdr(request, "X-Pocket-OTA", !FirmwareUpdate::capacity() ? "unavailable"
                                        : firmwareReady ? "staged" : "ready");
  return reply(request, "200 OK", sessionToken);
}

int hexDigit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

bool readFileName(httpd_req_t *request, char (&name)[kMaxFileName + 1]) {
  char encoded[kMaxFileName * 3 + 1];
  if (httpd_req_get_hdr_value_str(request, "X-Pocket-File", encoded, sizeof(encoded)) != ESP_OK) return false;
  size_t length = 0;
  for (const char *input = encoded; *input; ++input) {
    unsigned char character = *input;
    if (character == '%') {
      if (!input[1] || !input[2]) return false;
      const int high = hexDigit(input[1]);
      const int low = hexDigit(input[2]);
      if (high < 0 || low < 0) return false;
      character = (high << 4) | low;
      input += 2;
    }
    if (length == kMaxFileName || character < 32 || character == 127 ||
        strchr("/\\:\"<>|?*", character)) return false;
    name[length++] = char(character);
  }
  name[length] = '\0';
  return length && name[0] != '.' && name[length - 1] != '.' && name[length - 1] != ' ';
}

bool ensureDirectory(const char *path) {
  if (!strncmp(path, "/roms/", 6) && !SD.exists("/roms") && !SD.mkdir("/roms")) return false;
  if (!SD.exists(path) && !SD.mkdir(path)) return false;
  File directory = SD.open(path);
  return directory && directory.isDirectory();
}

bool receiveChunk(void *context, uint8_t *buffer, size_t remaining) {
  auto *request = static_cast<httpd_req_t *>(context);
  uint32_t receivedAt = millis();
  while (remaining && sessionState.load() != SessionState::Closing) {
    const int count = httpd_req_recv(request, reinterpret_cast<char *>(buffer), remaining);
    if (count == HTTPD_SOCK_ERR_TIMEOUT && millis() - receivedAt < kReceiveIdleMs) continue;
    if (count <= 0) return false;
    receivedAt = millis();
    lastActivity.store(receivedAt);
    remaining -= count;
    buffer += count;
  }
  return !remaining && sessionState.load() != SessionState::Closing;
}

enum class UploadKind { File, App, AppPackage };

esp_err_t receiveUpload(httpd_req_t *request, UploadKind kind) {
  if (!authorized(request)) return reject(request, "403 Forbidden", "Reload the transfer page.");
  if (!sdAvailable) return reject(request, "409 Conflict", "Insert a microSD card and restart transfer mode.");
  lastActivity.store(millis());
  char name[kMaxFileName + 1];
  if (!readFileName(request, name)) return reject(request, "400 Bad Request", "Invalid filename.");
  if (kind != UploadKind::File && !AppPackage::supportedName(name)) {
    return reject(request, "415 Unsupported Media Type", "Choose a .ppapp with a lowercase app ID.");
  }
  const auto destination = kind == UploadKind::File ? resolveDestination(name) :
      WifiTransfer::Destination{kind == UploadKind::App ? AppPackage::directory : AppPackage::inbox,
                                AppPackage::maxBytes};
  if (!destination.directory) return reject(request, "415 Unsupported Media Type", "Unsupported file type.");
  if (!request->content_len || request->content_len > destination.maxBytes) {
    return reject(request, "413 Content Too Large", "File is empty or over the size limit.");
  }
  const ActiveTransfer transfer;
  if (!transfer) return reject(request, "409 Conflict", "Transfer mode is closing.");
  if (!ensureDirectory(destination.directory)) {
    return reject(request, "500 Internal Server Error", "Could not open the folder on microSD.");
  }
  char output[kMaxFileName + 32];
  snprintf(output, sizeof(output), "/sd%s/%s", destination.directory, name);
  struct stat existing;
  if (stat(output, &existing) == 0) return reject(request, "409 Conflict", "That file already exists.");
  if (errno != ENOENT) return reject(request, "500 Internal Server Error", "Could not check the destination.");

  const int file = open(kTemporaryFile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (file < 0) return reject(request, "500 Internal Server Error", "Could not create the file on microSD.");
  const size_t total = request->content_len;
  size_t written = 0;
  uint32_t reportedAt = millis();
  bool writeFailed = false;
  Serial.printf("Wi-Fi upload: receiving %u bytes\n", unsigned(total));
  while (written < total) {
    const size_t count = std::min(total - written, kChunkBytes);
    if (!receiveChunk(request, uploadBuffer.get(), count)) break;
    if (write(file, uploadBuffer.get(), count) != static_cast<ssize_t>(count)) {
      writeFailed = true;
      break;
    }
    written += count;
    if (millis() - reportedAt >= 2000) {
      Serial.printf("Wi-Fi upload: %u/%u bytes written\n", unsigned(written), unsigned(total));
      reportedAt = millis();
    }
    // A buffered socket and SPI writes may otherwise keep the server runnable continuously.
    vTaskDelay(1);
  }
  const bool complete = written == total && sessionState.load() != SessionState::Closing;
  const bool synced = complete && fsync(file) == 0;
  const bool closed = close(file) == 0;
  const bool ready = synced && closed && sessionState.load() != SessionState::Closing;
  if (ready && kind == UploadKind::AppPackage && !AppPackage::validHeader(kTemporarySdPath)) {
    unlink(kTemporaryFile);
    return reject(request, "422 Unprocessable Content", "Invalid Pocket Play app or SDK version.");
  }
  if (ready && kind == UploadKind::App) {
    const auto result = AppPackage::install(kTemporarySdPath, name);
    unlink(kTemporaryFile);
    using Result = AppPackage::Result;
    switch (result) {
      case Result::Installed: return reply(request, "201 Created", output + 3);
      case Result::Invalid: return reject(request, "422 Unprocessable Content", "Invalid Pocket Play app or SDK version.");
      case Result::Exists: return reject(request, "409 Conflict", "An app with this ID is already installed.");
      case Result::StorageError: return reject(request, "500 Internal Server Error", "Could not install the app on microSD.");
    }
  }
  if (!ready || rename(kTemporaryFile, output) != 0) {
    unlink(kTemporaryFile);
    if (!complete && !writeFailed) {
      Serial.printf("Wi-Fi upload: interrupted at %u/%u bytes\n", unsigned(written), unsigned(total));
      return reject(request, "408 Request Timeout", "Upload interrupted.");
    }
    Serial.printf("Wi-Fi upload: SD save failed at %u/%u bytes\n", unsigned(written), unsigned(total));
    return reject(request, "500 Internal Server Error", "Could not save the file. Check the SD card and free space.");
  }
  Serial.printf("Wi-Fi upload: saved %u bytes\n", unsigned(written));
  return reply(request, "201 Created", output + 3);
}

esp_err_t upload(httpd_req_t *request) { return receiveUpload(request, UploadKind::File); }
esp_err_t installApp(httpd_req_t *request) { return receiveUpload(request, UploadKind::App); }
esp_err_t saveAppPackage(httpd_req_t *request) { return receiveUpload(request, UploadKind::AppPackage); }

bool receiveFirmware(void *context, uint8_t *buffer, size_t remaining) {
  const bool received = receiveChunk(context, buffer, remaining);
  if (received) vTaskDelay(1);
  return received;
}

esp_err_t firmware(httpd_req_t *request) {
  if (!authorized(request)) return reject(request, "403 Forbidden", "Reload the transfer page.");
  if (firmwareReady) return reject(request, "409 Conflict", "An update is ready. Restart Pocket Play.");
  char name[kMaxFileName + 1];
  if (!readFileName(request, name) || !FirmwareUpdate::supportedName(name)) {
    return reject(request, "415 Unsupported Media Type", "Choose the Pocket Play app image ending in .ino.bin.");
  }
  const ActiveTransfer transfer;
  if (!transfer) return reject(request, "409 Conflict", "Transfer mode is closing.");
  lastActivity.store(millis());
  using Result = FirmwareUpdate::Result;
  firmwareInstallPercent.store(0);
  const auto result = FirmwareUpdate::install(request->content_len, uploadBuffer.get(), kChunkBytes,
                                               receiveFirmware, request,
                                               [](int percent) { firmwareInstallPercent.store(percent); });
  if (result != Result::Ready) firmwareInstallPercent.store(-1);
  switch (result) {
    case Result::Ready:
      firmwareReady = true;
      lastActivity.store(millis());
      return reply(request, "201 Created", "Firmware ready. Restart console to apply it.");
    case Result::Unavailable:
      return reject(request, "409 Conflict", "This installation has no compatible update slot. Update over USB.");
    case Result::TooLarge:
      return reject(request, "413 Content Too Large", "Firmware exceeds the 3 MiB app slot.");
    case Result::WrongImage:
      return reject(request, "422 Unprocessable Content", "The firmware is corrupt or for a different build of Pocket Play.");
    case Result::Interrupted:
      return reject(request, "408 Request Timeout", "Firmware transfer interrupted. Retry the upload.");
    case Result::FlashError:
      return reject(request, "500 Internal Server Error", "Could not install firmware. Restart and try again.");
  }
  return ESP_FAIL;
}

esp_err_t restart(httpd_req_t *request) {
  if (!authorized(request)) return reject(request, "403 Forbidden", "Reload the transfer page.");
  if (request->content_len) return reject(request, "400 Bad Request", "No request body expected.");
  const esp_err_t sent = reply(request, "200 OK", "Restarting Pocket Play. Wi-Fi will turn off.");
  restartAt.store((millis() + 500) | 1);
  return sent;
}
}

bool WifiTransfer::loadSettings(Preferences &preferences) {
  store = &preferences;
  networkSaved = store->getBytesLength(kNetworkKey) == sizeof(network) &&
                 store->getBytes(kNetworkKey, &network, sizeof(network)) == sizeof(network) &&
                 validText(network.name, 1, maxNameBytes) &&
                 validText(network.password, 8, maxPasswordBytes);
  if (!networkSaved) {
    network = {};
    snprintf(network.name, sizeof(network.name), "PocketPlay-%04X", unsigned(ESP.getEfuseMac() >> 32) & 0xFFFF);
    snprintf(network.password, sizeof(network.password), "%08lX%04lX",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random() & 0xFFFF));
  }
  if (!store->getBool(kBootKey, false)) return false;
  if (!store->putBool(kBootKey, false)) {
    Serial.println("Wi-Fi: could not clear the one-time boot request.");
    return false;
  }
  return networkSaved;
}

bool WifiTransfer::scheduled() { return nextBoot; }

const char *WifiTransfer::schedule(bool enabled) {
  if (enabled && !networkSaved && !persistNetwork(network)) return "Could not save Wi-Fi settings.";
  if (!store->putBool(kBootKey, enabled)) return "Could not save Wi-Fi setting.";
  nextBoot = enabled;
  return nullptr;
}

const char *WifiTransfer::saveName(const char *name) {
  if (!validText(name, 1, maxNameBytes)) return "Use 1-32 characters.";
  NetworkCredentials updated = network;
  memset(updated.name, 0, sizeof(updated.name));
  strcpy(updated.name, name);
  return persistNetwork(updated) ? nullptr : "Could not save network name.";
}

const char *WifiTransfer::savePassword(const char *password) {
  if (!validText(password, 8, maxPasswordBytes)) return "Use 8-63 characters.";
  NetworkCredentials updated = network;
  memset(updated.password, 0, sizeof(updated.password));
  strcpy(updated.password, password);
  return persistNetwork(updated) ? nullptr : "Could not save password.";
}

const char *WifiTransfer::begin(ResolveDestination resolve, bool cardReady) {
  resolveDestination = resolve;
  sdAvailable = cardReady;
  firmwareReady = false;
  sessionState.store(SessionState::Idle);
  restartAt.store(0);
  firmwareInstallPercent.store(-1);
  if (sdAvailable) unlink(kTemporaryFile);  // Reclaim a partial upload left by a previous power loss.
  WiFi.persistent(false);
  if (!WiFi.softAP(network.name, network.password, 1, false, 1) ||
      !WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0))) {
    WiFi.mode(WIFI_OFF);
    return "Could not start Wi-Fi.";
  }
  for (size_t i = 0; i < sizeof(sessionToken) - 1; i += 8) {
    snprintf(sessionToken + i, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  }
  uploadBuffer.reset(new (std::nothrow) uint8_t[kChunkBytes]);
  if (!uploadBuffer) {
    stop();
    return "Not enough RAM for file transfer.";
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 6144;
  config.max_open_sockets = 3;
  config.max_req_hdr_len = 2048;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 1;
  config.send_wait_timeout = 2;
  if (httpd_start(&server, &config) != ESP_OK) {
    stop();
    return "Could not start file server.";
  }
  const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = page},
    {.uri = "/session", .method = HTTP_GET, .handler = session},
    {.uri = "/upload", .method = HTTP_POST, .handler = upload},
    {.uri = "/application", .method = HTTP_POST, .handler = installApp},
    {.uri = "/app-package", .method = HTTP_POST, .handler = saveAppPackage},
    {.uri = "/firmware", .method = HTTP_POST, .handler = firmware},
    {.uri = "/restart", .method = HTTP_POST, .handler = restart},
  };
  for (const auto &route : routes) {
    if (httpd_register_uri_handler(server, &route) != ESP_OK) {
      stop();
      return "Could not start file server.";
    }
  }
  lastActivity.store(millis());
  return nullptr;
}

const char *WifiTransfer::networkName() { return network.name; }
const char *WifiTransfer::password() { return network.password; }
const char *WifiTransfer::address() { return kAddress; }
int WifiTransfer::firmwareProgress() { return firmwareInstallPercent.load(); }

bool WifiTransfer::requestExit(bool buttonPressed) {
  const uint32_t now = millis();
  const uint32_t afterReply = restartAt.load();
  const bool browserRestart = afterReply && int32_t(now - afterReply) >= 0;
  if (!buttonPressed && !browserRestart && now - lastActivity.load() < kIdleMs) return false;
  auto expected = SessionState::Idle;
  if (!sessionState.compare_exchange_strong(expected, SessionState::Closing)) return false;
  Serial.printf("Wi-Fi: exiting transfer mode (%s)\n",
                browserRestart ? "browser restart" : buttonPressed ? "console controls" : "idle timeout");
  return true;
}

void WifiTransfer::stop() {
  sessionState.store(SessionState::Closing);
  if (server) {
    httpd_stop(server);
    server = nullptr;
  }
  uploadBuffer.reset();
  if (sdAvailable) unlink(kTemporaryFile);
  WiFi.mode(WIFI_OFF);
}
