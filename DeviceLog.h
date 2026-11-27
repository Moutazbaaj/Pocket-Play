#pragma once

#include <stdint.h>

class Preferences;

// Main-loop diagnostics. Capturing can outlive the on-screen diagnostics page.
namespace DeviceLog {
// Persisted activity codes. Preserve their numeric values when extending PPL1.
enum class Activity : uint8_t {
  Home, Library, Settings, About, Music, NowPlaying, MusicOptions, ScreenOff,
  Flashlight, GameProfile, Tools, MiniGames, Clock, Photos, Game, Paused, Error, Log,
  Applications, Application
};
enum class Music : uint8_t { Off, Playing, Paused, Suspended };

// On-disk record. Preserve its layout for PPL1 files.
struct Sample {
  uint32_t elapsedMs = 0;
  // Zero means voltage is unavailable. Track and charge use -1 for unavailable.
  uint16_t batteryMv = 0;
  int16_t track = -1;
  int8_t charge = -1;
  uint8_t volume = 0;
  uint8_t brightness = 0;
  uint8_t red = 0, green = 0, blue = 0;
  uint8_t peakLeft = 0, peakRight = 0;
  Activity activity = Activity::Home;
  Music music = Music::Off;
  uint8_t flashlight = 0;
  uint8_t gameSound = 0;
  uint8_t gameSystem = 0;
  uint8_t reserved[3]{};
};

// The Preferences object must remain open and alive while diagnostics is in use.
void begin(Preferences &preferences);
// Call after a successful SD mount to prepare /logs.
void prepareCard();
// Show the capture controls for the existing diagnostics session.
void open();
// Restore the active page after another screen or overlay drew over it.
void draw();
// Return false when the launcher should close diagnostics and return to About.
bool handle(uint16_t pressed, bool back, bool cardMounted);
// Return true when directional input may auto-repeat.
bool canRepeatScroll();
// Return true while the live buffer is collecting samples.
bool recording();
// Check whether a sample is due at the supplied millis() timestamp.
bool needsSample(uint32_t now);
// Call only when needsSample() is true. The logger supplies elapsedMs.
void append(uint32_t now, Sample sample);
// Run on the main loop, including while the log screen is hidden. Voltage is in millivolts.
void service(uint32_t now, bool cardMounted, bool visible, uint32_t batteryMv);
// End a capture and save it if a card is mounted.
void stop(uint32_t now, bool cardMounted);
// Queue the latest completed capture for serial output by service().
void requestSerial(bool cardMounted);
}
