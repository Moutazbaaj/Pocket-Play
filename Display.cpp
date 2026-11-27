#include "Display.h"
#include "Font8x8.h"
#include "Pins.h"
#include "src/gpsp/GbaCore.h"
#include <SPI.h>
#include <algorithm>
#include <driver/spi_master.h>
#include <esp_attr.h>

namespace {
constexpr Screen::ThemeColors kLightTheme = {0xF7DF, 0xFFFF, 0xEF7E, 0xD71E, 0xBEBD, 0x1106, 0x428C, 0x0350};
Screen::ThemeColors menuTheme = Screen::darkTheme;
bool lightThemeEnabled = false;
}

const Screen::ThemeColors &Screen::theme = menuTheme;

void Screen::setLightTheme(bool light) {
  lightThemeEnabled = light;
  menuTheme = light ? kLightTheme : darkTheme;
}

bool Screen::lightTheme() {
  return lightThemeEnabled;
}

uint16_t Screen::themeAccent(uint16_t color) {
  if (!lightThemeEnabled) return color;
  const uint16_t red = ((color >> 11) * 7) / 16;
  const uint16_t green = (((color >> 5) & 63) * 7) / 16;
  const uint16_t blue = ((color & 31) * 7) / 16;
  return (red << 11) | (green << 5) | blue;
}

namespace {
SPIClass displaySpi(FSPI);
uint32_t tftClockHz = kTftClockHz;
alignas(4) uint16_t line[Screen::width];
bool ready = false;

enum class PanelPower { On, Asleep, WakeRequested, Waking };
PanelPower panelPower = PanelPower::On;
uint32_t panelPowerChangedAt = 0;
constexpr uint32_t kPanelSleepTransitionMs = 120;
constexpr uint32_t kBacklightPwmHz = 4000;
constexpr uint8_t kBacklightPwmBits = 8;
uint8_t backlightPercent = 100;
bool backlightPwm = false;

// Apply the requested duty cycle with LEDC or release PWM for steady on and off states.
bool driveBacklight(uint8_t percent) {
  if (kTftBacklight < 0) {
    return false;
  }
  if (percent == 0 || percent == 100) {
    if (backlightPwm && !ledcDetach(kTftBacklight)) {
      return false;
    }
    backlightPwm = false;
    pinMode(kTftBacklight, OUTPUT);
    digitalWrite(kTftBacklight, percent == 100 ? HIGH : LOW);
    return true;
  }
  if (!backlightPwm) {
    backlightPwm = ledcAttach(kTftBacklight, kBacklightPwmHz, kBacklightPwmBits);
    if (!backlightPwm) {
      driveBacklight(panelPower == PanelPower::On ? 100 : 0);
      return false;
    }
  }
  constexpr unsigned maxDuty = (1u << kBacklightPwmBits) - 1;
  return ledcWrite(kTftBacklight, (maxDuty * percent + 50) / 100);
}

// 8 native rows or 6 scaled rows per SPI transfer.
constexpr size_t kGbaStripePixels = GBA_FRAME_WIDTH * 8;
constexpr uint32_t kGbaDmaMaxClockHz = 40000000;
static_assert(kGbaStripePixels == Screen::width * 6);
static_assert(GBA_FRAME_WIDTH * 4 == Screen::width * 3);
static_assert(GBA_FRAME_HEIGHT * 3 == Screen::height * 2);
static_assert(GBA_FRAME_HEIGHT % 8 == 0);
DMA_ATTR uint16_t gbaStripe[kGbaStripePixels];
spi_device_handle_t gbaDma = nullptr;
uint32_t gbaDmaClockHz = 0;
esp_err_t gbaDmaError = ESP_OK;

// Start a TFT transaction with the active transport and clock.
void beginWrite() {
  if (gbaDma) {
    gbaDmaError = ESP_OK;
  } else {
    displaySpi.beginTransaction(SPISettings(tftClockHz, MSBFIRST, SPI_MODE0));
  }
  digitalWrite(kTftCs, LOW);
}

// Finish the current TFT transaction and release chip select.
void endWrite() {
  digitalWrite(kTftCs, HIGH);
  if (!gbaDma) {
    displaySpi.endTransaction();
  }
}

// Send short command bytes synchronously on the ESP-IDF SPI transport.
void sendGbaDmaCommandBytes(const uint8_t *data, size_t length) {
  // ILI9341 window commands send at most four parameter bytes.
  while (length && gbaDmaError == ESP_OK) {
    spi_transaction_t transaction{};
    const size_t count = std::min(length, sizeof(transaction.tx_data));
    transaction.flags = SPI_TRANS_USE_TXDATA;
    transaction.length = count * 8;
    memcpy(transaction.tx_data, data, count);
    gbaDmaError = spi_device_transmit(gbaDma, &transaction);
    data += count;
    length -= count;
  }
}

// Toggle data/command for a panel opcode and send its optional parameters.
void command(uint8_t opcode, const uint8_t *data = nullptr, size_t length = 0) {
  digitalWrite(kTftDc, LOW);
  if (gbaDma) {
    sendGbaDmaCommandBytes(&opcode, 1);
    digitalWrite(kTftDc, HIGH);
    sendGbaDmaCommandBytes(data, length);
    return;
  }
  displaySpi.write(opcode);
  digitalWrite(kTftDc, HIGH);
  for (size_t i = 0; i < length; ++i) {
    displaySpi.write(data[i]);
  }
}

// Set an inclusive TFT address window and begin writing RGB565 pixels.
void setWindow(int x, int y, int w, int h) {
  const int right = x + w - 1;
  const int bottom = y + h - 1;
  const uint8_t columns[] = {uint8_t(x >> 8), uint8_t(x), uint8_t(right >> 8), uint8_t(right)};
  const uint8_t rows[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(bottom >> 8), uint8_t(bottom)};
  command(0x2A, columns, sizeof(columns));
  command(0x2B, rows, sizeof(rows));
  command(0x2C);
}

// Expand a 240-pixel GBA row to 320 pixels while preparing the display byte order.
void scaleGbaRow(const uint16_t *source, uint16_t *dest) {
  // Nearest-neighbor 240 to 320: three RGB565 pixels become four.
  for (int x = 0; x < GBA_FRAME_WIDTH; x += 3, dest += 4) {
    dest[0] = __builtin_bswap16(source[x]);
    dest[1] = dest[0];
    dest[2] = __builtin_bswap16(source[x + 1]);
    dest[3] = __builtin_bswap16(source[x + 2]);
  }
}

// Pack each frame stripe into the shared DMA buffer and transmit it before reusing the buffer.
void drawGbaDmaPixels(const uint16_t *pixels, bool fullScreen) {
  constexpr int w = GBA_FRAME_WIDTH;
  constexpr int h = GBA_FRAME_HEIGHT;
  if (fullScreen) {
    setWindow(0, 0, Screen::width, Screen::height);
  } else {
    setWindow((Screen::width - w) / 2, (Screen::height - h) / 2, w, h);
  }

  const int rowsPerStripe = fullScreen ? 4 : 8;
  for (int y = 0; y < h && gbaDmaError == ESP_OK; y += rowsPerStripe) {
    const uint16_t *source = pixels + y * w;
    if (fullScreen) {
      // Each pair of source rows becomes three output rows. Scale the first
      // row once and reuse it for the repeated output.
      constexpr size_t stride = Screen::width;
      for (int pair = 0; pair < 2; ++pair) {
        uint16_t *dest = gbaStripe + pair * 3 * stride;
        scaleGbaRow(source + pair * 2 * w, dest);
        memcpy(dest + stride, dest, stride * sizeof(*dest));
        scaleGbaRow(source + (pair * 2 + 1) * w, dest + 2 * stride);
      }
    } else {
      for (size_t i = 0; i < kGbaStripePixels; ++i) {
        gbaStripe[i] = __builtin_bswap16(source[i]);
      }
    }
    spi_transaction_t transaction{};
    transaction.length = sizeof(gbaStripe) * 8;
    transaction.tx_buffer = gbaStripe;
    gbaDmaError = spi_device_transmit(gbaDma, &transaction);
  }
}

}

// Initialize SPI, reset and configure the ILI9341, and set up panel orientation and backlight.
bool Screen::begin() {
  pinMode(kTftCs, OUTPUT);
  pinMode(kTftDc, OUTPUT);
  pinMode(kTftReset, OUTPUT);
  digitalWrite(kTftCs, HIGH);
  digitalWrite(kTftReset, LOW);
  delay(30);
  digitalWrite(kTftReset, HIGH);
  delay(120);

  ready = displaySpi.begin(kTftSck, -1, kTftMosi, kTftCs);
  if (!ready) {
    return false;
  }

  beginWrite();
  command(0x01);
  endWrite();
  delay(150);

  // ILI9341 settings from the N16R8 build, including its landscape orientation.
  const uint8_t init[][17] = {
    {0xCF, 3, 0x00, 0xC3, 0x30},
    {0xED, 4, 0x64, 0x03, 0x12, 0x81},
    {0xE8, 3, 0x85, 0x00, 0x78},
    {0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02},
    {0xF7, 1, 0x20}, {0xEA, 2, 0x00, 0x00},
    {0xC0, 1, 0x1B}, {0xC1, 1, 0x12}, {0xC5, 2, 0x3F, 0x3C},
    {0xC7, 1, 0x97}, {0x36, 1, kTftMadctl}, {0x3A, 1, 0x55},
    {0xB1, 2, 0x00, 0x10}, {0xB6, 2, 0x0A, 0xA2},
    {0xF6, 2, 0x01, 0x30}, {0xF2, 1, 0x00}, {0x26, 1, 0x01},
    {0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00},
    {0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F},
    {0x11, 0},
  };
  beginWrite();
  for (const auto &step : init) {
    command(step[0], &step[2], step[1]);
  }
  endWrite();
  delay(120);
  beginWrite();
  command(0x29);
  endWrite();

  panelPower = PanelPower::On;
  fill(0, 0, width, height, black);
  if (kTftBacklight >= 0) {
    driveBacklight(backlightPercent);
  }
  return true;
}

// Release Arduino TFT SPI pins before another peripheral drives a shared pin.
bool Screen::suspendSpi() {
  if (!ready || gbaDma) {
    return false;
  }
  displaySpi.end();
  // Keep the panel deselected while another peripheral drives its clock pin.
  pinMode(kTftCs, OUTPUT);
  digitalWrite(kTftCs, HIGH);
  ready = false;
  return true;
}

// Reattach the Arduino TFT bus after a caller releases the shared pin.
bool Screen::resumeSpi() {
  ready = displaySpi.begin(kTftSck, -1, kTftMosi, kTftCs);
  return ready;
}

// Turn off the backlight and send panel sleep commands, optionally waiting for the transition.
bool Screen::sleepPanel(bool waitForTransition) {
  if (panelPower == PanelPower::Asleep) {
    return true;
  }
  if (!ready || gbaDma || panelPower != PanelPower::On) {
    return false;
  }
  if (kTftBacklight >= 0 && !driveBacklight(0)) {
    return false;
  }
  beginWrite();
  command(0x28);  // Display off.
  command(0x10);  // Sleep in.
  endWrite();
  panelPowerChangedAt = millis();
  panelPower = PanelPower::Asleep;
  if (waitForTransition) {
    delay(kPanelSleepTransitionMs);
  }
  return true;
}

// Request exit from TFT sleep so finishPanelWake() can advance the timed transition.
void Screen::wakePanel() {
  if (!ready) {
    resumeSpi();
  }
  if (panelPower == PanelPower::Asleep) {
    panelPower = PanelPower::WakeRequested;
  }
}

// Advance the wake transition and restore saved brightness once the panel is ready.
bool Screen::finishPanelWake() {
  if (panelPower == PanelPower::On) {
    return true;
  }
  // The panel needs 120 ms in each direction. Poll so music can keep streaming.
  if (!ready || panelPower == PanelPower::Asleep ||
      millis() - panelPowerChangedAt < kPanelSleepTransitionMs) {
    return false;
  }
  if (panelPower == PanelPower::WakeRequested) {
    beginWrite();
    command(0x11);  // Sleep out.
    endWrite();
    panelPowerChangedAt = millis();
    panelPower = PanelPower::Waking;
    return false;
  }

  beginWrite();
  command(0x29);  // Display on.
  endWrite();
  panelPower = PanelPower::On;
  if (kTftBacklight >= 0) {
    driveBacklight(backlightPercent);
  }
  return true;
}

// Store a clamped brightness percentage and apply it when the panel is awake.
bool Screen::setBrightness(uint8_t percent) {
  if (percent < 1 || percent > 100 || panelPower != PanelPower::On ||
      !driveBacklight(percent)) {
    return false;
  }
  backlightPercent = percent;
  return true;
}

// Save the requested clock in hertz for subsequent TFT transactions.
void Screen::setClockHz(uint32_t clockHz) {
  tftClockHz = clockHz;
}

// Return the configured TFT clock in hertz.
uint32_t Screen::clockHz() {
  return gbaDma ? gbaDmaClockHz : tftClockHz;
}

// Write a solid RGB565 rectangle using the shared scanline buffer.
void Screen::fill(int x, int y, int w, int h, uint16_t color) {
  if (!ready || w <= 0 || h <= 0) {
    return;
  }
  std::fill_n(line, w, color);
  beginWrite();
  setWindow(x, y, w, h);
  for (int row = 0; row < h; ++row) {
    displaySpi.writePixels(line, w * sizeof(*line));
  }
  endWrite();
}

// Draw scaled foreground and background pixels for the firmware's fixed-width glyphs.
void Screen::text(int x, int y, const char *value, uint16_t fg, uint16_t bg, int scale) {
  const int columns = (width - x) / (8 * scale);
  const int count = min(int(strlen(value)), columns);
  if (!ready || count <= 0 || y + 8 * scale > height) {
    return;
  }

  const int w = count * 8 * scale;
  beginWrite();
  setWindow(x, y, w, 8 * scale);
  for (int row = 0; row < 8; ++row) {
    int offset = 0;
    for (int i = 0; i < count; ++i) {
      const unsigned c = static_cast<unsigned char>(value[i]);
      const uint8_t bits = kFont8x8[c >= 32 && c <= 126 ? c - 32 : '?' - 32][row];
      for (int bit = 0; bit < 8; ++bit) {
        const uint16_t color = bits & (0x80u >> bit) ? fg : bg;
        std::fill_n(line + offset, scale, color);
        offset += scale;
      }
    }
    for (int repeat = 0; repeat < scale; ++repeat) {
      displaySpi.writePixels(line, w * sizeof(*line));
    }
  }
  endWrite();
}

namespace {
constexpr int kCompactGlyphHeight = 7;

constexpr uint8_t kCompactFont[][kCompactGlyphHeight] = {
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // space
  {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},  // !
  {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},  // "
  {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},  // #
  {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},  // $
  {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},  // %
  {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},  // &
  {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},  // '
  {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // (
  {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // )
  {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},  // *
  {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},  // +
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08},  // ,
  {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06},  // .
  {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},  // /
  {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
  {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
  {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F},  // 2
  {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},  // 3
  {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
  {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},  // 5
  {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
  {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
  {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
  {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00},  // :
  {0x00, 0x06, 0x06, 0x00, 0x06, 0x04, 0x08},  // ;
  {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01},  // <
  {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},  // =
  {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10},  // >
  {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // ?
  {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E},  // @
  {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
  {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
  {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
  {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},  // D
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
  {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
  {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
  {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
  {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C},  // J
  {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
  {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
  {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},  // N
  {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
  {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
  {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
  {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
  {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
  {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},  // W
  {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
  {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},  // Y
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
  {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},  // [
  {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01},  // backslash
  {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},  // ]
  {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},  // ^
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},  // _
  {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00},  // `
  {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},  // a
  {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},  // b
  {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E},  // c
  {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},  // d
  {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0F},  // e
  {0x06, 0x09, 0x08, 0x1E, 0x08, 0x08, 0x08},  // f
  {0x00, 0x0F, 0x11, 0x0F, 0x01, 0x11, 0x0E},  // g
  {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},  // h
  {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},  // i
  {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},  // j
  {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},  // k
  {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // l
  {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},  // m
  {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11},  // n
  {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},  // o
  {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10},  // p
  {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01},  // q
  {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},  // r
  {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E},  // s
  {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06},  // t
  {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F},  // u
  {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},  // v
  {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A},  // w
  {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},  // x
  {0x00, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E},  // y
  {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},  // z
  {0x03, 0x04, 0x04, 0x08, 0x04, 0x04, 0x03},  // {
  {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // |
  {0x18, 0x04, 0x04, 0x02, 0x04, 0x04, 0x18},  // }
  {0x00, 0x00, 0x00, 0x09, 0x16, 0x00, 0x00},  // ~
};
static_assert(std::size(kCompactFont) == '~' - ' ' + 1);
}

// Center a compact label using the same glyph renderer as the scrolling text.
void Screen::compactLabel(int centerX, int y, const char *value, uint16_t fg, uint16_t bg) {
  const int w = int(strlen(value)) * compactTextAdvance - 1;
  if (w > 0) marqueeText(centerX - w / 2, y, w, value, 0, fg, bg, TextFont::Compact);
}

// Draw a clipped horizontal text window that wraps with a gap between copies.
void Screen::marqueeText(int x, int y, int w, const char *value, int offset,
                         uint16_t fg, uint16_t bg, TextFont font) {
  const bool compact = font == TextFont::Compact;
  const int advance = compact ? compactTextAdvance : 8;
  const int glyphHeight = compact ? kCompactGlyphHeight : 8;
  if (!ready || x < 0 || y < 0 || w <= 0 || x + w > width || y + glyphHeight > height) return;
  const int textWidth = int(strlen(value)) * advance;
  const int cycle = textWidth + marqueeGapPixels;
  beginWrite();
  setWindow(x, y, w, glyphHeight);
  for (int row = 0; row < glyphHeight; ++row) {
    int source = offset % cycle;
    int col = 0;
    while (col < w) {
      const int firstBit = source % advance;
      const int count = min(w - col, advance - firstBit);
      uint8_t bits = 0;
      if (source < textWidth) {
        const unsigned c = static_cast<unsigned char>(value[source / advance]);
        const unsigned index = c >= 32 && c <= 126 ? c - 32 : '?' - 32;
        bits = compact ? kCompactFont[index][row] << 3 : kFont8x8[index][row];
      }
      for (int bit = firstBit; bit < firstBit + count; ++bit) {
        line[col++] = bits & (0x80u >> bit) ? fg : bg;
      }
      source += count;
      if (source == cycle) source = 0;
    }
    displaySpi.writePixels(line, w * sizeof(*line));
  }
  endWrite();
}

// Transfer a prepared RGB565 rectangle in a single TFT window.
void Screen::drawRgb565(const uint16_t *pixels, int x, int y, int w, int h) {
  if (!ready || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      w > width - x || h > height - y) return;

  beginWrite();
  setWindow(x, y, w, h);
  displaySpi.writePixels(pixels, w * h * sizeof(*pixels));
  endWrite();
}

// Present the 160 by 144 GB or GBC frame at the selected display scale.
void Screen::drawGameFrame(const void *pixels, GbScale scale) {
  if (!ready) {
    return;
  }
  constexpr int sourceWidth = 160;
  constexpr int sourceHeight = 144;
  const auto *source = static_cast<const uint16_t *>(pixels);

  if (scale == GbScale::Native) {
    beginWrite();
    setWindow((width - sourceWidth) / 2, (height - sourceHeight) / 2, sourceWidth, sourceHeight);
    displaySpi.writePixels(source, sourceWidth * sourceHeight * sizeof(*source));
    endWrite();
    return;
  }

  constexpr int fitWidth = 266;
  static_assert(width == sourceWidth * 2);
  static_assert(fitWidth % 2 == 0);
  const int targetWidth = scale == GbScale::Fit ? fitWidth : width;
  const int targetX = (width - targetWidth) / 2;
  // Fit preserves the original Game Boy aspect ratio; Full uses every pixel.
  beginWrite();
  setWindow(targetX, 0, targetWidth, height);
  const uint16_t *previousRow = nullptr;
  for (int y = 0; y < height; ++y) {
    const auto *row = source + (y * sourceHeight / height) * sourceWidth;
    if (row != previousRow) {
      if (scale == GbScale::Full) {
        // At 320 pixels, each source pixel is repeated exactly twice.
        for (int x = 0; x < sourceWidth; ++x) {
          const uint16_t pixel = __builtin_bswap16(row[x]);
          line[2 * x] = pixel;
          line[2 * x + 1] = pixel;
        }
      } else {
        for (int x = 0; x < targetWidth; ++x) {
          line[x] = __builtin_bswap16(row[x * sourceWidth / fitWidth]);
        }
      }
      previousRow = row;
    }
    // Pixels are already in TFT byte order. Send each row as a raw byte block.
    displaySpi.writeBytes(reinterpret_cast<const uint8_t *>(line), targetWidth * sizeof(*line));
  }
  endWrite();
}

// Scale indexed pixels as they go to the panel, avoiding a second RGB565 framebuffer.
void Screen::drawCartridgeFrame(const CartridgeFrame &frame, GbScale scale) {
  if (!ready || (!frame.rgb565 && (!frame.pixels || !frame.palette)) || frame.width <= 0 || frame.height <= 0 ||
      frame.width > width || frame.height > height || frame.pitch < frame.width) return;
  int w = frame.width;
  int h = frame.height;
  if (scale == GbScale::Full) {
    w = width;
    h = height;
  } else if (scale == GbScale::Fit) {
    if (frame.width * height >= frame.height * width) {
      w = width;
      h = frame.height * width / frame.width;
    } else {
      h = height;
      w = frame.width * height / frame.height;
    }
  }

  const int left = (width - w) / 2;
  const int top = (height - h) / 2;
  // Sega display modes can change height during play. Clear only the newly exposed margins.
  static int previousWidth = 0;
  static int previousHeight = 0;
  if (w != previousWidth || h != previousHeight) {
    if (w < width) {
      fill(0, top, left, h, black);
      fill(left + w, top, width - left - w, h, black);
    }
    if (h < height) {
      fill(0, 0, width, top, black);
      fill(0, top + h, width, height - top - h, black);
    }
    previousWidth = w;
    previousHeight = h;
  }

  // Precalculate the horizontal lookup once per frame. Consecutive duplicated rows
  // can reuse the converted RGB565 scanline directly.
  uint16_t sourceColumns[width];
  for (int x = 0; x < w; ++x) sourceColumns[x] = x * frame.width / w;
  beginWrite();
  setWindow(left, top, w, h);
  int previousRow = -1;
  for (int y = 0; y < h; ++y) {
    const int row = y * frame.height / h;
    if (row != previousRow) {
      if (frame.rgb565) {
        const auto *pixels = frame.rgb565 + row * frame.pitch;
        if (w == frame.width) {
          memcpy(line, pixels, w * sizeof(*line));
        } else {
          for (int x = 0; x < w; ++x) line[x] = pixels[sourceColumns[x]];
        }
      } else {
        const auto *pixels = frame.pixels + row * frame.pitch;
        for (int x = 0; x < w; ++x) line[x] = frame.palette[pixels[sourceColumns[x]]];
      }
      previousRow = row;
    }
    displaySpi.writePixels(line, w * sizeof(*line));
  }
  endWrite();
}

// Switch TFT SPI to ESP-IDF DMA with the DMA-specific clock limit and restore Arduino SPI on failure.
bool Screen::beginGbaVideo() {
  if (gbaDma) {
    return true;
  }
  if (!ready) {
    return false;
  }

  // SPI2 is the write-only TFT. The SD reader stays on SPI3.
  // Arduino SPI and the IDF driver need exclusive ownership of SPI2.
  displaySpi.end();
  spi_bus_config_t bus{};
  bus.mosi_io_num = kTftMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kTftSck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
  bus.max_transfer_sz = sizeof(gbaStripe);
  bus.isr_cpu_id = ESP_INTR_CPU_AFFINITY_0;

  esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (error == ESP_OK) {
    spi_device_interface_config_t device{};
    // DMA clocks each stripe continuously over the TFT's GPIO-matrix wiring.
    gbaDmaClockHz = std::min(tftClockHz, kGbaDmaMaxClockHz);
    device.clock_speed_hz = gbaDmaClockHz;
    device.mode = 0;
    device.spics_io_num = -1;  // Manual CS spans the whole LCD frame.
    device.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
    device.queue_size = 1;
    error = spi_bus_add_device(SPI2_HOST, &device, &gbaDma);
    if (error == ESP_OK) {
      int actualKHz = 0;
      if (spi_device_get_actual_freq(gbaDma, &actualKHz) == ESP_OK) {
        gbaDmaClockHz = uint32_t(actualKHz) * 1000;
        Serial.printf("GBA display DMA: %d kHz actual SPI clock\n", actualKHz);
      }
      return true;
    }
    spi_bus_free(SPI2_HOST);
  }

  gbaDma = nullptr;
  Serial.printf("GBA display DMA unavailable (%s). Restoring Arduino SPI.\n", esp_err_to_name(error));
  ready = displaySpi.begin(kTftSck, -1, kTftMosi, kTftCs);
  return false;
}

// Release the DMA transport and restore Arduino SPI for normal menu drawing.
void Screen::endGbaVideo() {
  if (gbaDma) {
    spi_bus_remove_device(gbaDma);
    spi_bus_free(SPI2_HOST);
    gbaDma = nullptr;
    ready = displaySpi.begin(kTftSck, -1, kTftMosi, kTftCs);
  }
}

// Present a centered native frame or stretch it to fill the panel using the active transport.
bool Screen::drawGbaFrame(const uint16_t *pixels, bool fullScreen) {
  if (!ready) {
    return false;
  }
  constexpr int w = GBA_FRAME_WIDTH;
  constexpr int h = GBA_FRAME_HEIGHT;
  beginWrite();
  if (gbaDma) {
    drawGbaDmaPixels(pixels, fullScreen);
    endWrite();
    if (gbaDmaError == ESP_OK) {
      return true;
    }
    Serial.printf("GBA display DMA failed (%s). Restoring Arduino SPI.\n", esp_err_to_name(gbaDmaError));
    endGbaVideo();
    return false;
  }
  if (!fullScreen) {
    setWindow((width - w) / 2, (height - h) / 2, w, h);
    displaySpi.writePixels(pixels, w * h * sizeof(*pixels));
  } else {
    // This sends twice as many pixels as native GBA output, so it is optional.
    setWindow(0, 0, width, height);
    for (int y = 0; y < height; ++y) {
      const auto *row = pixels + (y * h / height) * w;
      for (int x = 0; x < width; ++x) {
        line[x] = row[x * w / width];
      }
      displaySpi.writePixels(line, width * sizeof(*line));
    }
  }
  endWrite();
  return true;
}
