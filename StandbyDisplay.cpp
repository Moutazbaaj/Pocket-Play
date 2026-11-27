#include "StandbyDisplay.h"

#include <Preferences.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <esp_timer.h>
#include <iterator>
#include <sys/time.h>

#include "Display.h"
#include "MenuStyle.h"

extern "C" {
#include "src/gnuboy/gnuboy.h"
}


namespace {
enum class Face : uint8_t { Digital, Analog, Minimal };
enum class Row : uint8_t { Face, TimeFormat, Year, Month, Day, Hour, Minute, ApplyTime, Minutes, Timer, Back };

constexpr const char *kFaces[] = {"DIGITAL", "ANALOG", "MINIMAL"};
constexpr int kFirstYear = 2000;
constexpr int kLastYear = 2099;
constexpr int kRowCount = int(Row::Back) + 1;
constexpr int kClockX = Screen::width / 2;
constexpr int kClockY = 111;
constexpr int64_t kUsPerSecond = 1000000;
uint16_t accent() { return Screen::themeAccent(0x7DFF); }

Preferences *store = nullptr;
Face face = Face::Digital;
bool twelveHour = false;
bool setupOpen = false;
Row selection = Row::Face;
std::tm editedTime{};
uint8_t timerMinutes = 5;
int64_t timerDeadline = 0;
bool setupTimerRunning = false;
time_t lastClockSecond = -1;
int lastTimerSecond = -2;
int lastClockDay = -1;
bool lastClockPm = false;

struct Point {
  int x, y;
};
Point lastHands[3]{};
bool analogHandsVisible = false;

// Convert wall time to a calendar only when the device clock has a usable date.
bool readClock(time_t now, std::tm &clock) {
  return localtime_r(&now, &clock) && clock.tm_year >= kFirstYear - 1900;
}

// Return the valid day count for the selected calendar month and year.
int daysInMonth(int year, int month) {
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return days[month] + (month == 1 && leap ? 1 : 0);
}

// Step a setting through an inclusive range, wrapping at either end.
int wrapValue(int value, int low, int high, int step) {
  if (value + step < low) return high;
  if (value + step > high) return low;
  return value + step;
}

// Center a fixed-width text line on the display.
void centeredText(int y, const char *text, uint16_t color, int scale = 1) {
  const int left = (Screen::width - int(strlen(text)) * 8 * scale) / 2;
  Screen::text(left, y, text, color, Screen::theme.background, scale);
}

// Return the countdown rounded up to seconds using the monotonic timer.
int remainingTimerSeconds() {
  if (!timerDeadline) return -1;
  const int64_t remaining = timerDeadline - esp_timer_get_time();
  return remaining > 0 ? int((remaining + kUsPerSecond - 1) / kUsPerSecond) : 0;
}

// Convert a position around the dial into pixel coordinates at the given radius.
Point clockPoint(int position, int positions, int radius) {
  constexpr float kTwoPi = 6.283185307f;
  const float angle = kTwoPi * position / positions;
  return {kClockX + int(lroundf(radius * sinf(angle))),
          kClockY - int(lroundf(radius * cosf(angle)))};
}

// Draw the analog face's stationary hour markers.
void drawClockMarks() {
  for (int hour = 0; hour < 12; ++hour) {
    const Point tick = clockPoint(hour, 12, 55);
    const int size = hour % 3 == 0 ? 5 : 3;
    Screen::fill(tick.x - size / 2, tick.y - size / 2, size, size,
                 hour % 3 == 0 ? accent() : Screen::theme.muted);
  }
}

// Render an analog hand from the center to its endpoint.
void drawHand(Point end, uint16_t color, int width) {
  const int steps = std::max(abs(end.x - kClockX), abs(end.y - kClockY));
  for (int step = 0; step <= steps; ++step) {
    const int x = kClockX + (end.x - kClockX) * step / steps;
    const int y = kClockY + (end.y - kClockY) * step / steps;
    Screen::fill(x - width / 2, y - width / 2, width, width, color);
  }
}

// Erase the old hands and draw the updated hands and center cap.
void drawAnalogHands(const std::tm &clock) {
  constexpr int widths[] = {3, 2, 1};
  const uint16_t colors[] = {Screen::theme.text, Screen::theme.text, accent()};
  if (analogHandsVisible) {
    for (int i = 0; i < 3; ++i) drawHand(lastHands[i], Screen::theme.background, widths[i]);
  }
  lastHands[0] = clockPoint((clock.tm_hour % 12) * 60 + clock.tm_min, 720, 30);
  lastHands[1] = clockPoint(clock.tm_min * 60 + clock.tm_sec, 3600, 40);
  lastHands[2] = clockPoint(clock.tm_sec, 60, 48);
  for (int i = 0; i < 3; ++i) drawHand(lastHands[i], colors[i], widths[i]);
  Screen::fill(kClockX - 2, kClockY - 2, 5, 5, accent());
  analogHandsVisible = true;
}

// Refresh the selected clock face and date when their displayed values change.
void updateClock(bool force) {
  const time_t now = time(nullptr);
  if (!force && now == lastClockSecond) return;

  std::tm clock{};
  const bool clockSet = readClock(now, clock);
  if (force || (clockSet && (face != Face::Minimal || now / 60 != lastClockSecond / 60))) {
    char value[16] = {};
    if (face == Face::Analog) {
      if (force) drawClockMarks();
      if (clockSet) drawAnalogHands(clock);
      else centeredText(108, "SET TIME", Screen::theme.muted);
    } else {
      const bool minimal = face == Face::Minimal;
      const char *pattern;
      if (twelveHour) pattern = minimal ? "%I:%M" : "%I:%M:%S";
      else pattern = minimal ? "%H:%M" : "%H:%M:%S";
      if (clockSet) std::strftime(value, sizeof(value), pattern, &clock);
      centeredText(minimal ? 84 : 92, clockSet ? value : (minimal ? "--:--" : "--:--:--"),
                   Screen::theme.text, minimal ? 6 : 4);
    }
  }

  const bool isPm = clock.tm_hour >= 12;
  if (twelveHour && clockSet && (force || isPm != lastClockPm)) {
    centeredText(face == Face::Analog ? 170 : 138, isPm ? "PM" : "AM", accent());
    lastClockPm = isPm;
  }

  if (force || (clockSet && clock.tm_yday != lastClockDay)) {
    char date[32] = {};
    if (clockSet) std::strftime(date, sizeof(date), "%a, %d %b %Y", &clock);
    centeredText(face == Face::Analog ? 182 : 154, clockSet ? date : "USE MENU TO SET DATE / TIME",
                 accent());
    lastClockDay = clock.tm_yday;
  }
  lastClockSecond = now;
}

// Refresh the countdown line when its remaining seconds or finished state changes.
void updateTimer(bool force) {
  const int seconds = remainingTimerSeconds();
  if (!force && seconds == lastTimerSecond) return;
  char value[24];
  if (seconds > 0) snprintf(value, sizeof(value), "TIMER %02d:%02d", seconds / 60, seconds % 60);
  else snprintf(value, sizeof(value), " %s ", seconds == 0 ? "TIMER DONE" : "TIMER OFF");

  // Clear only when changing between the countdown and its status labels.
  if (!force && (seconds <= 0 || lastTimerSecond <= 0)) {
    Screen::fill(96, 201, 128, 8, Screen::theme.background);
  }
  centeredText(201, value, seconds < 0 ? Screen::theme.muted : accent());
  lastTimerSecond = seconds;
}

// Render one setup control and its current value.
void drawSetupRow(Row row) {
  constexpr const char *labels[] = {
    "CLOCK FACE", "TIME FORMAT", "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SET DATE / TIME",
    "TIMER LENGTH", "COUNTDOWN", "BACK"
  };
  static_assert(std::size(labels) == kRowCount);
  const int index = int(row);
  const int y = 52 + index * 15;
  const bool selected = selection == row;
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
  drawCard(6, y, 308, 14, selected, accent());
  Screen::text(16, y + 3, labels[index], fg, bg);

  char value[24] = {};
  switch (row) {
    case Row::Face: snprintf(value, sizeof(value), "< %s >", kFaces[int(face)]); break;
    case Row::TimeFormat: snprintf(value, sizeof(value), "< %s HOUR >", twelveHour ? "12" : "24"); break;
    case Row::Year: snprintf(value, sizeof(value), "< %04d >", editedTime.tm_year + 1900); break;
    case Row::Month: snprintf(value, sizeof(value), "< %02d >", editedTime.tm_mon + 1); break;
    case Row::Day: snprintf(value, sizeof(value), "< %02d >", editedTime.tm_mday); break;
    case Row::Hour:
      if (twelveHour) {
        const int hour = (editedTime.tm_hour + 11) % 12 + 1;
        snprintf(value, sizeof(value), "< %02d %s >", hour, editedTime.tm_hour < 12 ? "AM" : "PM");
      } else {
        snprintf(value, sizeof(value), "< %02d >", editedTime.tm_hour);
      }
      break;
    case Row::Minute: snprintf(value, sizeof(value), "< %02d >", editedTime.tm_min); break;
    case Row::ApplyTime: snprintf(value, sizeof(value), "APPLY >"); break;
    case Row::Minutes: snprintf(value, sizeof(value), "< %u MIN >", unsigned(timerMinutes)); break;
    case Row::Timer: snprintf(value, sizeof(value), "%s >", setupTimerRunning ? "STOP" : "START"); break;
    case Row::Back: snprintf(value, sizeof(value), ">"); break;
  }
  Screen::text(231 - int(strlen(value)) * 4, y + 3, value, selected ? accent() : fg, bg);
}

// Draw the clock setup navigation hints.
void drawSetupFooter() {
  Screen::fill(0, 221, Screen::width, 18, Screen::theme.background);
  centeredText(224, twelveHour ? "A ON HOUR: AM/PM   B / MENU: CLOCK" : "B / MENU: CLOCK",
               Screen::theme.muted);
}

// Initialize editable date and time fields from the clock and open the setup screen.
void openSetup() {
  setupOpen = true;
  selection = Row::Face;
  const time_t now = time(nullptr);
  if (!readClock(now, editedTime) || editedTime.tm_year > kLastYear - 1900) {
    editedTime = {};
    editedTime.tm_year = 2026 - 1900;
    editedTime.tm_mday = 1;
    editedTime.tm_hour = 12;
  }
  StandbyDisplay::draw();
}

// Adjust the selected setup field and persist user preferences where applicable.
void changeSetupValue(int step) {
  switch (selection) {
    case Row::Face:
      face = static_cast<Face>(wrapValue(int(face), 0, int(std::size(kFaces)) - 1, step));
      store->putUChar("standbyFace", uint8_t(face));
      break;
    case Row::TimeFormat:
      twelveHour = !twelveHour;
      store->putBool("standby12h", twelveHour);
      drawSetupRow(Row::Hour);
      drawSetupFooter();
      break;
    case Row::Year:
      editedTime.tm_year = wrapValue(editedTime.tm_year, kFirstYear - 1900, kLastYear - 1900, step);
      break;
    case Row::Month: editedTime.tm_mon = wrapValue(editedTime.tm_mon, 0, 11, step); break;
    case Row::Day:
      editedTime.tm_mday = wrapValue(editedTime.tm_mday, 1,
                                    daysInMonth(editedTime.tm_year + 1900, editedTime.tm_mon), step);
      break;
    case Row::Hour: editedTime.tm_hour = wrapValue(editedTime.tm_hour, 0, 23, step); break;
    case Row::Minute: editedTime.tm_min = wrapValue(editedTime.tm_min, 0, 59, step); break;
    case Row::Minutes:
      timerMinutes = uint8_t(wrapValue(timerMinutes, 1, 99, step));
      store->putUChar("standbyTimer", timerMinutes);
      break;
    default: return;
  }
  drawSetupRow(selection);
  if (selection == Row::Year || selection == Row::Month) {
    const int day = std::min(editedTime.tm_mday, daysInMonth(editedTime.tm_year + 1900, editedTime.tm_mon));
    if (day != editedTime.tm_mday) {
      editedTime.tm_mday = day;
      drawSetupRow(Row::Day);
    }
  }
}

// Return to the clock view with a full refresh of the selected face.
void closeSetup() {
  setupOpen = false;
  StandbyDisplay::draw();
}

// Apply edited time, toggle the countdown, or leave setup according to the selected action.
void activateSetupRow() {
  if (selection == Row::ApplyTime) {
    std::tm newTime = editedTime;
    newTime.tm_sec = 0;
    newTime.tm_isdst = -1;
    const timeval value{std::mktime(&newTime), 0};
    if (value.tv_sec < 0 || settimeofday(&value, nullptr) != 0) {
      Screen::fill(0, 221, Screen::width, 18, Screen::theme.background);
      centeredText(224, "COULD NOT SET CLOCK", accent());
      return;
    }
  } else if (selection == Row::Hour && twelveHour) {
    editedTime.tm_hour = (editedTime.tm_hour + 12) % 24;
    drawSetupRow(Row::Hour);
    return;
  } else if (selection == Row::Timer) {
    timerDeadline = remainingTimerSeconds() > 0 ? 0 :
                    esp_timer_get_time() + int64_t(timerMinutes) * 60 * kUsPerSecond;
  } else if (selection != Row::Back) {
    return;
  }
  closeSetup();
}
}

// Restore the chosen clock face, time format, and timer duration from preferences.
void StandbyDisplay::begin(Preferences &preferences) {
  store = &preferences;
  const uint8_t savedFace = store->getUChar("standbyFace", 0);
  face = savedFace < std::size(kFaces) ? static_cast<Face>(savedFace) : Face::Digital;
  timerMinutes = std::clamp<uint8_t>(store->getUChar("standbyTimer", 5), 1, 99);
  twelveHour = store->getBool("standby12h", false);
}

// Open the Clock app and reset its drawing cache.
void StandbyDisplay::open() {
  setupOpen = false;
  draw();
}

// Format the current time using the user's 12-hour or 24-hour choice.
void StandbyDisplay::formatTime(char *value, size_t size) {
  std::tm clock{};
  if (!readClock(time(nullptr), clock) ||
      !std::strftime(value, size, twelveHour ? "%I:%M %p" : "%H:%M", &clock)) {
    snprintf(value, size, "--:--");
  }
}

// Refresh the visible clock, timer, or setup page.
void StandbyDisplay::draw() {
  if (setupOpen) {
    drawHeader("CLOCK OPTIONS", "A: SELECT   LEFT/RIGHT: CHANGE", accent());
    setupTimerRunning = remainingTimerSeconds() > 0;
    for (int row = 0; row < kRowCount; ++row) drawSetupRow(static_cast<Row>(row));
    drawSetupFooter();
    return;
  }
  drawHeader("CLOCK", kFaces[int(face)], accent());
  analogHandsVisible = false;
  updateClock(true);
  updateTimer(true);
  centeredText(224, "MENU: OPTIONS   B: HOME", Screen::theme.muted);
}

// Route Clock controls and report when the launcher should leave the app.
bool StandbyDisplay::handle(uint16_t pressed, bool menuPressed) {
  if (!setupOpen) {
    if (pressed & GB_PAD_B) return false;
    if (menuPressed) openSetup();
    else {
      updateClock(false);
      updateTimer(false);
    }
    return true;
  }
  if (menuPressed || (pressed & GB_PAD_B)) {
    closeSetup();
  } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const Row previous = selection;
    selection = static_cast<Row>(wrapValue(int(selection), 0, kRowCount - 1, pressed & GB_PAD_UP ? -1 : 1));
    drawSetupRow(previous);
    drawSetupRow(selection);
  } else if (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)) {
    changeSetupValue(pressed & GB_PAD_LEFT ? -1 : 1);
  } else if (pressed & (GB_PAD_A | GB_PAD_START)) {
    activateSetupRow();
  }

  const bool timerRunning = remainingTimerSeconds() > 0;
  if (setupOpen && timerRunning != setupTimerRunning) {
    setupTimerRunning = timerRunning;
    drawSetupRow(Row::Timer);
  }
  return true;
}

// Report whether the face is visible so the launcher can coordinate idle behavior.
bool StandbyDisplay::showingClock() {
  return !setupOpen;
}
