#include "UsbControls.h"
#include "HidGamepad.h"

#include <Arduino.h>

#include "Pins.h"

#if POCKETPLAY_USB_CONTROLS && defined(CONFIG_IDF_TARGET_ESP32S3) && !ARDUINO_USB_CDC_ON_BOOT && \
    !ARDUINO_USB_MSC_ON_BOOT && !ARDUINO_USB_DFU_ON_BOOT && __has_include("tusb.h")
#include "tusb.h"
#if CFG_TUH_ENABLED && CFG_TUH_HID
#define POCKETPLAY_USB_HOST 1
#endif
#endif

#if defined(POCKETPLAY_USB_HOST)
#include <atomic>
#include <esp_private/usb_phy.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "src/gpsp/GbaCore.h"
extern "C" {
#include "src/gnuboy/gnuboy.h"
}

namespace {
constexpr uint16_t kMenu = 0x100;
constexpr uint8_t kRhport = 0;  // ESP32-S3's internal, full-speed OTG controller.
TaskHandle_t task = nullptr;
SemaphoreHandle_t taskStopped = nullptr;
std::atomic<bool> stopRequested{false};
ControllerButtons input;
uint8_t device = 0;
uint8_t interfaceIndex = 0;
bool keyboard = false;
HidGamepad gamepad;

uint16_t keyboardInput(const uint8_t *report, uint16_t length) {
  if (length < sizeof(hid_keyboard_report_t)) return 0;
  uint16_t held = 0;
  for (int i = 2; i < 8; ++i) {
    switch (report[i]) {
      case HID_KEY_ARROW_RIGHT: case HID_KEY_D: held |= GB_PAD_RIGHT; break;
      case HID_KEY_ARROW_LEFT: case HID_KEY_A: held |= GB_PAD_LEFT; break;
      case HID_KEY_ARROW_UP: case HID_KEY_W: held |= GB_PAD_UP; break;
      case HID_KEY_ARROW_DOWN: case HID_KEY_S: held |= GB_PAD_DOWN; break;
      case HID_KEY_Z: case HID_KEY_J: case HID_KEY_ENTER: held |= GB_PAD_A; break;
      case HID_KEY_X: case HID_KEY_K: case HID_KEY_BACKSPACE: held |= GB_PAD_B; break;
      case HID_KEY_I: held |= GB_PAD_START; break;
      case HID_KEY_U: held |= GB_PAD_SELECT; break;
      case HID_KEY_M: case HID_KEY_ESCAPE: held |= kMenu; break;
      case HID_KEY_Q: held |= GBA_PAD_L; break;
      case HID_KEY_E: held |= GBA_PAD_R; break;
      default: break;
    }
  }
  return held;
}

void usbTask(void *) {
  usb_phy_handle_t handle = nullptr;
  const usb_phy_config_t phy = {
    .controller = USB_PHY_CTRL_OTG,
    .target = USB_PHY_TARGET_INT,
    .otg_mode = USB_OTG_MODE_HOST,
  };
  if (usb_new_phy(&phy, &handle) == ESP_OK && tuh_init(kRhport)) {
    while (!stopRequested.load(std::memory_order_relaxed)) {
      tuh_task_ext(2, false);
      if (device && tuh_hid_receive_ready(device, interfaceIndex)) tuh_hid_receive_report(device, interfaceIndex);
      vTaskDelay(1);
    }
    tuh_deinit(kRhport);
  } else {
    Serial.println("USB controls could not start. Use UART for Serial and free the native USB port.");
  }
  if (handle) usb_del_phy(handle);
  device = 0;
  input.clear();
  xSemaphoreGive(taskStopped);
  vTaskDelete(nullptr);
}
}

extern "C" void tuh_hid_mount_cb(uint8_t address, uint8_t iface, const uint8_t *descriptor, uint16_t length) {
  if (device) return;
  keyboard = tuh_hid_interface_protocol(address, iface) == HID_ITF_PROTOCOL_KEYBOARD;
  if (!keyboard && !gamepad.load(descriptor, length, HidGamepad::Mapping::Generic)) return;
  device = address;
  interfaceIndex = iface;
  input.clear();
}

extern "C" void tuh_hid_umount_cb(uint8_t address, uint8_t iface) {
  if (address != device || iface != interfaceIndex) return;
  device = 0;
  input.clear();
}

extern "C" void tuh_hid_report_received_cb(uint8_t address, uint8_t iface, const uint8_t *report, uint16_t length) {
  if (address != device || iface != interfaceIndex) return;
  input.publish(keyboard ? keyboardInput(report, length) : gamepad.usbReport(report, length));
}

void UsbControls::begin() {
  if (task) return;
  if (!taskStopped) taskStopped = xSemaphoreCreateBinary();
  stopRequested.store(false, std::memory_order_relaxed);
  if (taskStopped && xTaskCreate(usbTask, "usb-input", 6144, nullptr, 2, &task) != pdPASS) task = nullptr;
}

void UsbControls::stop() {
  if (!task) return;
  stopRequested.store(true, std::memory_order_relaxed);
  xSemaphoreTake(taskStopped, portMAX_DELAY);
  task = nullptr;
}

UsbControls::Input UsbControls::takeInput() {
  return input.take();
}
#else
void UsbControls::begin() {}
void UsbControls::stop() {}
UsbControls::Input UsbControls::takeInput() { return {}; }
#endif
