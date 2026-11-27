#pragma once

#include "ControllerInput.h"

namespace UsbControls {
using Input = ControllerInput;

// Own the native USB host when the Arduino build assigns Serial to UART.
void begin();
void stop();
// Read held buttons and consume press edges from the USB input task.
Input takeInput();
}
