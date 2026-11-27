#include "DeviceLog.h"

#include <Preferences.h>
#include <SD.h>
#include <algorithm>
#include <ctime>
#include <iterator>
#include <esp_heap_caps.h>
#include <memory>
#include <vector>

#include "Display.h"
#include "MenuStyle.h"

extern "C" {
#include "src/gnuboy/gnuboy.h"
}


namespace {
constexpr char kLogDir[] = "/logs";
constexpr char kPendingPath[] = "/logs/.capture.tmp";
constexpr uint32_t kMagic = 0x314C5050;  // PPL1
constexpr uint32_t kSampleMs = 100;
constexpr uint16_t kDurations[] = {30, 60, 120, 180, 240, 300};
constexpr uint32_t kMaxSamples = 300000 / kSampleMs + 1;
constexpr int kRows = 8;
constexpr uint32_t kMaxSavedLogsListed = 512;

using DeviceLog::Sample;
struct Header {
  uint32_t magic = kMagic;
  uint32_t count = 0;
  uint32_t requestedMs = 0;
  uint32_t durationMs = 0;
  int64_t started = 0;
  uint16_t baselineMv = 0;
  uint16_t minimumMv = 0;
  uint16_t maximumMv = 0;
  uint16_t reserved = 0;
};
static_assert(sizeof(Header) == 32 && sizeof(Sample) == 24);
using Samples = std::unique_ptr<Sample[], decltype(&heap_caps_free)>;
struct Session {
  Header header;
  Samples samples{nullptr, heap_caps_free};
};
Session capture;
Session latest;
String latestFile;
bool active = false;
uint32_t startedAt = 0;
uint32_t sampledAt = 0;
uint8_t durationIndex = 1;
Preferences *store = nullptr;

enum class Page { Menu, History, Detail };
Page page = Page::Menu;
int menuSelection = 0;
int historySelection = 0;
std::vector<String> history;
String detailPath;
Header detailHeader;
uint32_t detailIndex = 0;
uint16_t activeStatusSecond = UINT16_MAX;
uint32_t liveBatteryMv = 0;
uint32_t voltageDrawnAt = 0;

Session serialSession;
int serialIndex = -1;
size_t serialOffset = 0;
char serialLine[224] = {};

// Convert a captured activity code to its display and CSV label.
const char *activityName(DeviceLog::Activity activity) {
  constexpr const char *names[] = {
    "HOME", "LIBRARY", "SETTINGS", "ABOUT", "MUSIC", "NOW PLAYING", "MUSIC OPTIONS",
    "SCREEN OFF", "FLASHLIGHT", "GAME PROFILE", "DEVICE TOOLS", "MINI GAMES", "CLOCK",
    "PHOTOS", "GAME", "PAUSED", "ERROR", "LOG", "APPLICATIONS", "APPLICATION",
  };
  const unsigned index = unsigned(activity);
  return index < std::size(names) ? names[index] : "UNKNOWN";
}

// Convert a captured music state to its display and CSV label.
const char *musicName(DeviceLog::Music music) {
  switch (music) {
    case DeviceLog::Music::Off: return "OFF";
    case DeviceLog::Music::Playing: return "PLAYING";
    case DeviceLog::Music::Paused: return "PAUSED";
    case DeviceLog::Music::Suspended: return "SUSPENDED";
  }
  return "UNKNOWN";
}

// Allocate a bounded sample array in PSRAM when available and release it through the ESP heap API.
Samples allocateSamples(size_t count) {
  const size_t bytes = count * sizeof(Sample);
  auto *buffer = static_cast<Sample *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<Sample *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buffer) std::uninitialized_default_construct_n(buffer, count);
  return Samples(buffer, heap_caps_free);
}

// Validate the magic, sample count, and exact file length before reading any sample records.
bool readHeader(File &file, Header &header) {
  return file && file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
         header.magic == kMagic && header.count && header.count <= kMaxSamples &&
         file.size() == sizeof(header) + header.count * sizeof(Sample);
}

// Recognize a numbered capture path while ignoring temporary files and unrelated card contents.
bool isLogPath(const String &path) {
  const int name = path.lastIndexOf('/') + 1;
  if (name == 0 || path.length() < unsigned(name + 13) || path[name + 8] != '_' || !path.endsWith(".ppl")) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if (path[name + i] < '0' || path[name + i] > '9') return false;
  }
  return true;
}

// Collect and sort completed capture paths, with the highest session numbers first.
void readHistory() {
  history.clear();
  File dir = SD.open(kLogDir, FILE_READ);
  if (!dir || !dir.isDirectory()) return;
  for (;;) {
    bool directory = false;
    String path = dir.getNextFileName(&directory);
    if (path.isEmpty()) break;
    if (directory || !isLogPath(path)) continue;
    if (history.size() < kMaxSavedLogsListed) {
      history.push_back(std::move(path));
    } else {
      auto oldest = std::min_element(history.begin(), history.end());
      if (path > *oldest) *oldest = std::move(path);
    }
  }
  std::sort(history.rbegin(), history.rend());
}

// Write the capture to a temporary file and publish it under its next numbered name when complete.
String saveSession(const Session &session) {
  if (!SD.exists(kLogDir) && !SD.mkdir(kLogDir)) return {};
  File dir = SD.open(kLogDir, FILE_READ);
  if (!dir || !dir.isDirectory()) return {};
  unsigned next = 1;
  for (;;) {
    String path = dir.getNextFileName();
    if (path.isEmpty()) break;
    if (!isLogPath(path)) continue;
    const char *name = strrchr(path.c_str(), '/');
    if (!name) continue;
    const unsigned id = unsigned(strtoul(name + 1, nullptr, 10));
    next = std::max(next, id + 1);
  }
  dir.close();
  char date[24] = "undated";
  std::tm clock{};
  const std::time_t when = std::time_t(session.header.started);
  if (localtime_r(&when, &clock) && clock.tm_year >= 100) {
    std::strftime(date, sizeof(date), "%Y-%m-%d_%H-%M-%S", &clock);
  }
  char path[64];
  if (next > 99999999) return {};
  snprintf(path, sizeof(path), "/logs/%08u_%s.ppl", next, date);
  if (SD.exists(path)) return {};
  File file = SD.open(kPendingPath, FILE_WRITE);
  if (!file) return {};
  const size_t bytes = session.header.count * sizeof(Sample);
  const bool written =
      file.write(reinterpret_cast<const uint8_t *>(&session.header), sizeof(Header)) == sizeof(Header) &&
      file.write(reinterpret_cast<const uint8_t *>(session.samples.get()), bytes) == bytes;
  file.close();
  if (written && SD.rename(kPendingPath, path)) return String(path);
  SD.remove(kPendingPath);
  return {};
}

// Allocate enough space for the selected duration and record wall-clock and monotonic start times.
bool start(uint32_t now) {
  const uint32_t durationMs = uint32_t(kDurations[durationIndex]) * 1000;
  auto samples = allocateSamples(durationMs / kSampleMs + 1);
  if (!samples) return false;
  capture = Session{};
  capture.header.requestedMs = durationMs;
  capture.header.started = std::time(nullptr);
  capture.samples = std::move(samples);
  startedAt = now;
  active = true;
  return true;
}

// Read and validate a full capture into a separate session for serial export.
bool loadSession(const String &path, Session &session) {
  File file = SD.open(path, FILE_READ);
  Header header;
  if (!readHeader(file, header)) return false;
  auto samples = allocateSamples(header.count);
  if (!samples) return false;
  const size_t bytes = header.count * sizeof(Sample);
  if (file.read(reinterpret_cast<uint8_t *>(samples.get()), bytes) != bytes) return false;
  session.header = header;
  session.samples = std::move(samples);
  return true;
}

// Format the latest battery reading in millivolts or label an unavailable ADC reading.
void formatVoltageNow(char *out, size_t size, uint32_t millivolts) {
  if (millivolts) snprintf(out, size, "Voltage now: %u mV", unsigned(millivolts));
  else snprintf(out, size, "Voltage now: unavailable");
}

// Repaint the live voltage line and record when it was updated.
void drawMenuVoltage() {
  char voltage[32];
  formatVoltageNow(voltage, sizeof(voltage), liveBatteryMv);
  Screen::fill(12, 181, 208, 8, Screen::theme.background);
  Screen::text(12, 181, voltage, Screen::theme.text, Screen::theme.background);
  voltageDrawnAt = millis();
}

// Repaint the recording countdown or report where the latest capture was saved.
void drawMenuStatus() {
  char line[40];
  if (active) {
    const unsigned seconds = (millis() - startedAt) / 1000;
    activeStatusSecond = seconds;
    snprintf(line, sizeof(line), "Recording %us / %u s", seconds, unsigned(capture.header.requestedMs / 1000));
  } else if (latest.samples) {
    snprintf(line, sizeof(line), "Last log: %s", latestFile.isEmpty() ? "in memory" : "saved to SD");
  } else {
    snprintf(line, sizeof(line), "Ready to record");
  }
  Screen::fill(10, 161, 300, 11, Screen::theme.background);
  Screen::text(12, 162, line, Screen::theme.accent, Screen::theme.background);
}

// Draw the diagnostics controls, capture status, and latest voltage.
void drawMenu() {
  drawHeader("DIAGNOSTIC LOG", "A: SELECT   LEFT/RIGHT: DURATION", Screen::theme.accent);
  const char *labels[] = {"DURATION", active ? "STOP CAPTURE" : "START CAPTURE", "HISTORY"};
  const unsigned seconds = kDurations[durationIndex];
  char duration[20];
  snprintf(duration, sizeof(duration), "< %u %s >", seconds < 60 ? seconds : seconds / 60,
           seconds < 60 ? "SEC" : "MIN");
  for (int i = 0; i < 3; ++i) {
    const int y = 55 + i * 33;
    const bool selected = i == menuSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(8, y, 304, 27, selected, Screen::theme.accent);
    Screen::text(18, y + 9, labels[i], Screen::theme.text, bg);
    Screen::text(209, y + 9, i ? "OPEN >" : duration, Screen::theme.muted, bg);
  }
  drawMenuStatus();
  drawMenuVoltage();
  Screen::text(228, 181, "10 Hz", Screen::theme.muted, Screen::theme.background);
  Screen::text(12, 199, "SERIAL: LOG PRINTS THE LATEST CSV", Screen::theme.muted, Screen::theme.background);
  Screen::text(12, 224, "B / MENU: ABOUT", Screen::theme.muted, Screen::theme.background);
}

// Check whether the most recent capture needs a separate history entry because it is only in RAM.
bool memoryEntry() { return latest.samples && latestFile.isEmpty(); }
// Count the history rows, including a capture kept only in RAM.
int historyCount() { return int(history.size()) + int(memoryEntry()); }

// Draw the visible window of capture history around the selected session.
void drawHistory() {
  drawHeader("LOG HISTORY", "A: VIEW   B / MENU: BACK", Screen::theme.accent);
  const int count = historyCount();
  if (!count) {
    Screen::text(12, 85, "NO COMPLETED LOGS", Screen::theme.muted, Screen::theme.background);
    return;
  }
  const int first = historySelection / kRows * kRows;
  for (int i = first; i < std::min(count, first + kRows); ++i) {
    const int y = 53 + (i - first) * 20;
    const bool selected = i == historySelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(8, y, 304, 18, selected, Screen::theme.accent);
    if (memoryEntry() && i == 0) {
      Screen::text(15, y + 5, "LATEST CAPTURE (IN MEMORY)", Screen::theme.text, bg);
    } else {
      const String &path = history[i - int(memoryEntry())];
      const char *name = strrchr(path.c_str(), '/') + 1;
      char label[35];
      snprintf(label, sizeof(label), "%.*s", int(strlen(name)) - 4, name);
      Screen::text(15, y + 5, label, Screen::theme.text, bg);
    }
  }
}

// Fetch one indexed sample directly from the selected file or the in-memory session.
bool readDetail(Sample &sample) {
  if (detailPath.isEmpty()) {
    if (!latest.samples || detailIndex >= latest.header.count) return false;
    sample = latest.samples[detailIndex];
    detailHeader = latest.header;
    return true;
  }
  File file = SD.open(detailPath, FILE_READ);
  if (!readHeader(file, detailHeader) || detailIndex >= detailHeader.count) return false;
  return file.seek(uint32_t(sizeof(Header) + detailIndex * sizeof(Sample))) &&
         file.read(reinterpret_cast<uint8_t *>(&sample), sizeof(sample)) == sizeof(sample);
}

// Draw the session summary and selected sample, preserving the header when moving between samples.
void drawDetail(bool firstOpen) {
  if (firstOpen) drawHeader("LOG SAMPLE", "UP/DOWN: STEP   LEFT/RIGHT: 1 SEC", Screen::theme.accent);
  Screen::fill(8, 50, 304, 167, Screen::theme.background);
  Sample sample{};
  if (!readDetail(sample)) {
    Screen::text(12, 73, "COULD NOT READ THIS CAPTURE", Screen::theme.muted, Screen::theme.background);
  } else {
    char lines[9][37];
    snprintf(lines[0], sizeof(lines[0]), "%lu.%03lu s   SAMPLE %lu/%lu",
             (unsigned long)(sample.elapsedMs / 1000), (unsigned long)(sample.elapsedMs % 1000),
             (unsigned long)(detailIndex + 1), (unsigned long)detailHeader.count);
    const int baseline = detailHeader.baselineMv;
    char delta[9], charge[8];
    formatVoltageNow(lines[1], sizeof(lines[1]), sample.batteryMv);
    if (sample.batteryMv && baseline) snprintf(delta, sizeof(delta), "%+d", int(sample.batteryMv) - baseline);
    else snprintf(delta, sizeof(delta), "--");
    if (sample.charge >= 0) snprintf(charge, sizeof(charge), "%d%%", sample.charge);
    else snprintf(charge, sizeof(charge), "--");
    snprintf(lines[2], sizeof(lines[2]), "Change %s mV    Charge %s", delta, charge);
    snprintf(lines[3], sizeof(lines[3]), "SCREEN: %s", activityName(sample.activity));
    if (sample.track >= 0) {
      snprintf(lines[4], sizeof(lines[4]), "MUSIC: %s   TRACK %d", musicName(sample.music), sample.track + 1);
    } else {
      snprintf(lines[4], sizeof(lines[4]), "MUSIC: %s", musicName(sample.music));
    }
    snprintf(lines[5], sizeof(lines[5]), "VOL %u%%   MUSIC PEAK L%u/R%u", sample.volume, sample.peakLeft, sample.peakRight);
    snprintf(lines[6], sizeof(lines[6]), "BACKLIGHT %u%%    GAME SOUND %s", sample.brightness, sample.gameSound ? "ON" : "OFF");
    snprintf(lines[7], sizeof(lines[7]), "LIGHT %s   RGB %u/%u/%u", sample.flashlight ? "ON" : "OFF", sample.red, sample.green, sample.blue);
    if (baseline) {
      snprintf(lines[8], sizeof(lines[8]), "MIN/MAX %u/%u mV   USB: N/A", detailHeader.minimumMv, detailHeader.maximumMv);
    } else {
      snprintf(lines[8], sizeof(lines[8]), "MIN/MAX: N/A   USB: N/A");
    }
    for (int i = 0; i < 9; ++i) Screen::text(12, 55 + i * 17, lines[i], Screen::theme.text, Screen::theme.background);
  }
  if (firstOpen) Screen::text(12, 224, "B / MENU: HISTORY", Screen::theme.muted, Screen::theme.background);
}

// Format the CSV header or the next sample into the serial transmit buffer.
void fillSerialLine() {
  if (serialIndex == -1) {
    snprintf(serialLine, sizeof(serialLine),
             "elapsed_ms,view,music,track,volume_pct,screen_pct,flashlight,red,green,blue,music_peak_l,music_peak_r,game_sound_active,game_system,battery_mV,charge_pct,delta_mV,current_mA,battery_mW,usb\n");
    return;
  }
  const auto &s = serialSession.samples[serialIndex];
  char mv[8] = "NA", charge[8] = "NA", delta[9] = "NA", track[8] = "NA";
  if (s.batteryMv) {
    snprintf(mv, sizeof(mv), "%u", s.batteryMv);
    if (serialSession.header.baselineMv) {
      snprintf(delta, sizeof(delta), "%d", int(s.batteryMv) - serialSession.header.baselineMv);
    }
  }
  if (s.charge >= 0) snprintf(charge, sizeof(charge), "%d", s.charge);
  if (s.track >= 0) snprintf(track, sizeof(track), "%d", s.track + 1);
  constexpr const char *systems[] = {"NA", "GB", "GBC", "GBA", "NES", "SMS", "GG", "GENESIS"};
  const char *system = s.gameSystem < std::size(systems) ? systems[s.gameSystem] : "NA";
  snprintf(serialLine, sizeof(serialLine), "%lu,%s,%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,NA,NA,NA\n",
           (unsigned long)s.elapsedMs, activityName(s.activity), musicName(s.music), track, s.volume,
           s.brightness, s.flashlight, s.red, s.green, s.blue, s.peakLeft, s.peakRight, s.gameSound,
           system, mv, charge, delta);
}

// Send the portion of the current CSV line that fits in the serial transmit buffer.
void serviceSerial() {
  if (!serialSession.samples) return;
  const int available = Serial.availableForWrite();
  if (available <= 0) return;
  if (!serialOffset) fillSerialLine();
  const size_t length = strlen(serialLine);
  const size_t written = Serial.write(reinterpret_cast<const uint8_t *>(serialLine + serialOffset),
                                      std::min(length - serialOffset, size_t(available)));
  serialOffset += written;
  if (serialOffset == length) {
    serialOffset = 0;
    if (++serialIndex >= int(serialSession.header.count)) serialSession = Session{};
  }
}
}

// Load the recorded duration preference and retain the shared preferences handle.
void DeviceLog::begin(Preferences &preferences) {
  store = &preferences;
  const uint8_t value = store->getUChar("logDuration", 1);
  durationIndex = value < std::size(kDurations) ? value : 1;
}

// Create the log directory on the mounted SD card if needed.
void DeviceLog::prepareCard() {
  if (!SD.exists(kLogDir)) SD.mkdir(kLogDir);
}

// Enter the diagnostics controls without stopping a running capture.
void DeviceLog::open() {
  menuSelection = 0;
  page = Page::Menu;
  draw();
}

// Redraw the active diagnostics page when the launcher restores it.
void DeviceLog::draw() {
  switch (page) {
    case Page::Menu: drawMenu(); break;
    case Page::History: drawHistory(); break;
    case Page::Detail: drawDetail(true); break;
  }
}

// Route navigation and capture controls, returning false when Back should return to About.
bool DeviceLog::handle(uint16_t pressed, bool back, bool cardMounted) {
  if (back) {
    if (page == Page::Menu) return false;
    page = page == Page::Detail ? Page::History : Page::Menu;
    draw();
    return true;
  }
  const bool activate = pressed & (GB_PAD_A | GB_PAD_START);
  const bool vertical = pressed & (GB_PAD_UP | GB_PAD_DOWN);
  const bool horizontal = pressed & (GB_PAD_LEFT | GB_PAD_RIGHT);
  const int step = pressed & (GB_PAD_UP | GB_PAD_LEFT) ? -1 : 1;
  if (page == Page::Menu) {
    if (vertical) {
      menuSelection = (menuSelection + step + 3) % 3;
    } else if (menuSelection == 0 && horizontal && !active) {
      const int count = std::size(kDurations);
      durationIndex = (durationIndex + step + count) % count;
      store->putUChar("logDuration", durationIndex);
    } else if (menuSelection == 1 && activate) {
      if (active) stop(millis(), cardMounted);
      else if (!start(millis())) {
        Screen::fill(10, 161, 300, 11, Screen::theme.background);
        Screen::text(12, 162, "INSUFFICIENT MEMORY TO RECORD", Screen::theme.text, Screen::theme.background);
        return true;
      }
    } else if (menuSelection == 2 && activate) {
      history.clear();
      if (cardMounted) readHistory();
      historySelection = 0;
      page = Page::History;
    } else {
      return true;
    }
    draw();
  } else if (page == Page::History && historyCount()) {
    if (vertical) {
      const int next = std::clamp(historySelection + step, 0, historyCount() - 1);
      if (next != historySelection) {
        historySelection = next;
        drawHistory();
      }
    } else if (activate) {
      if (memoryEntry() && historySelection == 0) {
        detailPath = "";
        detailHeader = latest.header;
      } else {
        detailPath = history[historySelection - int(memoryEntry())];
      }
      detailIndex = 0;
      page = Page::Detail;
      drawDetail(true);
    }
  } else if (page == Page::Detail && (vertical || horizontal) && detailHeader.count) {
    const int next = std::clamp(int(detailIndex) + step * (horizontal ? 10 : 1), 0, int(detailHeader.count) - 1);
    if (next != int(detailIndex)) {
      detailIndex = next;
      drawDetail(false);
    }
  }
  return true;
}

// Allow held directions to repeat in the history and sample views.
bool DeviceLog::canRepeatScroll() { return page != Page::Menu; }
// Report whether a capture is active, including while another app is open.
bool DeviceLog::recording() { return active; }

// Check elapsed time and remaining buffer capacity before the caller collects telemetry.
bool DeviceLog::needsSample(uint32_t now) {
  return active && now - startedAt <= capture.header.requestedMs &&
         capture.header.count < capture.header.requestedMs / kSampleMs + 1 &&
         (!capture.header.count || now - sampledAt >= kSampleMs);
}

// Store a due sample and update the voltage baseline, minimum, and maximum.
void DeviceLog::append(uint32_t now, Sample sample) {
  sample.elapsedMs = now - startedAt;
  auto &header = capture.header;
  if (sample.batteryMv) {
    if (!header.baselineMv) header.baselineMv = header.minimumMv = sample.batteryMv;
    header.minimumMv = std::min(header.minimumMv, sample.batteryMv);
    header.maximumMv = std::max(header.maximumMv, sample.batteryMv);
  }
  capture.samples[header.count++] = sample;
  sampledAt = now;
}

// Finish the current capture, keep it in RAM, and attempt to save it to the mounted card.
void DeviceLog::stop(uint32_t now, bool cardMounted) {
  if (!active) return;
  active = false;
  if (!capture.header.count) {
    capture = Session{};
    return;
  }
  capture.header.durationMs = std::min(now - startedAt, capture.header.requestedMs);
  latest = std::move(capture);
  latestFile = cardMounted ? saveSession(latest) : String();
  Serial.printf("\nLog finished: %lu samples, %s. Type log to print CSV.\n",
                (unsigned long)latest.header.count, latestFile.isEmpty() ? "in memory" : latestFile.c_str());
}

// Stop a completed capture, refresh visible status, and advance a pending serial export.
void DeviceLog::service(uint32_t now, bool cardMounted, bool visible, uint32_t batteryMv) {
  liveBatteryMv = batteryMv;
  if (active && now - startedAt >= capture.header.requestedMs) {
    stop(now, cardMounted);
    if (visible && page == Page::Menu) {
      drawMenu();
    } else if (visible && page == Page::History) {
      if (cardMounted) readHistory();
      historySelection = 0;
      drawHistory();
    }
  }
  if (visible && page == Page::Menu) {
    if (active && (now - startedAt) / 1000 != activeStatusSecond) drawMenuStatus();
    if (millis() - voltageDrawnAt >= 1000) drawMenuVoltage();
  }
  serviceSerial();
}

// Snapshot the last completed capture for serial output, loading the newest SD log when needed.
void DeviceLog::requestSerial(bool cardMounted) {
  serialSession = Session{};
  if (latest.samples) {
    auto samples = allocateSamples(latest.header.count);
    if (samples) {
      std::copy_n(latest.samples.get(), latest.header.count, samples.get());
      serialSession = {latest.header, std::move(samples)};
    }
  } else if (cardMounted) {
    File dir = SD.open(kLogDir, FILE_READ);
    String newest;
    if (dir && dir.isDirectory()) {
      for (;;) {
        String path = dir.getNextFileName();
        if (path.isEmpty()) break;
        if (isLogPath(path) && path > newest) newest = std::move(path);
      }
    }
    dir.close();
    if (!newest.isEmpty()) loadSession(newest, serialSession);
  }
  if (!serialSession.samples) {
    Serial.println("No completed log available to print.");
    return;
  }
  serialIndex = -1;
  serialOffset = 0;
  const auto &header = serialSession.header;
  char date[24] = "unknown";
  const std::time_t when = std::time_t(header.started);
  std::tm clock{};
  if (localtime_r(&when, &clock) && clock.tm_year >= 100) {
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &clock);
  }
  Serial.printf("\nLog started %s, duration %lu ms, %lu samples.\n", date,
                (unsigned long)header.durationMs, (unsigned long)header.count);
}
