#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

// Decode a standard Desktop gamepad report map. USB prepends report IDs while BLE carries them separately.
class HidGamepad {
 public:
  enum class Mapping { Generic, XboxBle };
  bool load(const uint8_t *descriptor, size_t length, Mapping mapping);
  bool hasInputs(uint8_t reportId) const;
  uint16_t report(const uint8_t *data, size_t length, uint8_t id);
  uint16_t usbReport(const uint8_t *data, size_t length);

 private:
  struct Field {
    unsigned offset = 0;
    unsigned bits = 0;
    int32_t minimum = 0;
    int32_t maximum = 0;
  };
  struct Report {
    uint8_t id = 0;
    unsigned bits = 0;
    Field x, y, hat;
    std::array<Field, 11> buttons;
    uint16_t held = 0;
    bool mapped = false;
  };
  static int32_t readField(const uint8_t *data, const Field &field);
  static uint16_t axisDirection(const uint8_t *data, const Field &axis, uint16_t low, uint16_t high);
  std::array<Report, 8> reports_;
  unsigned count_ = 0;
  bool usesIds_ = false;
};
