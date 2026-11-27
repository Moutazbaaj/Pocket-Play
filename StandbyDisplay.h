#pragma once

#include <stddef.h>
#include <stdint.h>

class Preferences;

// Clock app state, timer, and status-clock formatting.
namespace StandbyDisplay {
// Load settings from a Preferences object that remains open while the app runs.
void begin(Preferences &preferences);
// Open the current clock face and reset rendering state.
void open();
// Write a null-terminated header time, or --:-- before the device clock is set.
void formatTime(char *value, size_t size);
// Refresh the clock or its setup screen.
void draw();
// Process input and return false when the launcher should close the Clock app.
bool handle(uint16_t pressed, bool menuPressed);
// Return true on the clock face, including when its countdown is running.
bool showingClock();
}
