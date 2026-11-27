#pragma once

#include <stdint.h>

class Preferences;

namespace PhotoViewer {
// Load the saved browser layout from the launcher's Preferences namespace.
void begin(Preferences &preferences);
void open(bool cardMounted);
void draw();
// Return false when the launcher should leave Photos.
bool handle(uint16_t pressed, bool menuPressed, bool rescan);
bool canRepeatScroll();
// Publish any decoded image and schedule the next visible image when appropriate.
void service(bool controlsActive, bool musicPlaying);
// Cancel and drain an SD job before a card remount, sleep, or deleting a file.
void stop();
}
