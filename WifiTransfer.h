#pragma once

#include <stddef.h>

class Preferences;

namespace WifiTransfer {
constexpr size_t maxNameBytes = 32;
constexpr size_t maxPasswordBytes = 63;

struct Destination {
  const char *directory = nullptr;
  size_t maxBytes = 0;
};

using ResolveDestination = Destination (*)(const char *name);

// Load the credentials and consume the one-time transfer boot request.
bool loadSettings(Preferences &preferences);
bool scheduled();
const char *schedule(bool enabled);
const char *saveName(const char *name);
const char *savePassword(const char *password);

// Start an isolated access point. The caller owns the SD card for the session.
const char *begin(ResolveDestination resolve, bool cardReady);
const char *networkName();
const char *password();
const char *address();
// Read the current firmware install percentage, or -1 outside an install.
int firmwareProgress();
// Claim a restart only while the file server is idle.
bool requestExit(bool buttonPressed);
// Stop accepting requests and close any partial upload before restarting.
void stop();
}
