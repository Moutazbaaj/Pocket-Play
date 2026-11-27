#pragma once

#include "ControllerInput.h"

class Preferences;

namespace BluetoothControls {
constexpr int maxDevices = 5;
enum class State { Off, RestartRequired, Starting, Ready, Scanning, Pairing, Connected, Disconnecting, Error, StartupFailed };
struct Snapshot {
  State state = State::Off;
  const char *status = "Bluetooth is off";
  char devices[maxDevices][25] = {};
  char connected[25] = {};
  uint32_t revision = 0;
  int count = 0;
  bool enabled = false;
};

void begin(Preferences &preferences, bool disableAtBoot = false);
void enableOnRestart();
void disable();
void suspend();
void resume();
void service();
bool scan();
bool pair(int device);
bool disconnect();
Snapshot status();
ControllerInput takeInput();
}
