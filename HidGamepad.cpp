#include "HidGamepad.h"

#include "src/gpsp/GbaCore.h"
extern "C" {
#include "src/gnuboy/gnuboy.h"
}

namespace {
constexpr uint16_t kMenu = 0x100;
constexpr unsigned kMaxReportBits = 8 * 64;
constexpr uint16_t kPadButtons[] = {
  GB_PAD_A, GB_PAD_B, GBA_PAD_L, GBA_PAD_R, GB_PAD_SELECT, GB_PAD_START, kMenu,
  GB_PAD_UP, GB_PAD_DOWN, GB_PAD_RIGHT, GB_PAD_LEFT,
};
constexpr uint32_t kGenericUsages[] = {
  0x90001, 0x90002, 0x90005, 0x90006, 0x90009, 0x9000A, 0x9000B,
  0x10090, 0x10091, 0x10092, 0x10093,
};
constexpr uint32_t kXboxUsages[] = {
  0x90001, 0x90002, 0x90007, 0x90008, 0x9000B, 0x9000C, 0x9000D,
  0x10090, 0x10091, 0x10092, 0x10093,
};
static_assert(std::size(kPadButtons) == std::size(kGenericUsages) &&
              std::size(kPadButtons) == std::size(kXboxUsages));

int32_t signExtend(uint32_t value, unsigned bits) {
  if (bits == 32) return static_cast<int32_t>(value);
  const uint32_t sign = uint32_t(1) << (bits - 1);
  return static_cast<int32_t>((value ^ sign) - sign);
}

}

// Parse only standard Desktop joystick/gamepad collections and variable input fields.
bool HidGamepad::load(const uint8_t *descriptor, size_t length, Mapping mapping) {
  reports_.fill(Report{});
  count_ = 0;
  usesIds_ = false;
  const auto &buttonUsages = mapping == Mapping::XboxBle ? kXboxUsages : kGenericUsages;
  if (!descriptor || !length) return false;
  struct Globals {
    uint32_t page = 0, size = 0, count = 0;
    int32_t minimum = 0, maximum = 0;
    uint8_t id = 0;
  } global;
  std::array<Globals, 4> saved;
  unsigned stack = 0;
  std::array<uint32_t, 16> usage{};
  unsigned localCount = 0;
  uint32_t firstUsage = 0, lastUsage = 0;
  unsigned depth = 0, gamepadDepth = 0;
  bool usable = false;

  for (size_t at = 0; at < length;) {
    const uint8_t prefix = descriptor[at++];
    if (prefix == 0xFE) {
      if (length - at < 2) return false;
      const unsigned skip = descriptor[at];
      at += 2;
      if (skip > length - at) return false;
      at += skip;
      continue;
    }
    const unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3;
    if (size > length - at) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint32_t(descriptor[at++]) << (8 * i);
    const unsigned item = prefix & 0xFC;
    const uint32_t localUsage = size == 4 ? value : (global.page << 16) | value;
    switch (item) {
      case 0x04: global.page = value; break;
      case 0x14: global.minimum = size ? signExtend(value, size * 8) : 0; break;
      case 0x24: global.maximum = global.minimum < 0 && size ? signExtend(value, size * 8) : int32_t(value); break;
      case 0x74: global.size = value; break;
      case 0x84:
        if (!value || value > 255) return false;
        global.id = uint8_t(value);
        usesIds_ = true;
        break;
      case 0x94: global.count = value; break;
      case 0xA4:
        if (stack == saved.size()) return false;
        saved[stack++] = global;
        break;
      case 0xB4:
        if (!stack) return false;
        global = saved[--stack];
        break;
      case 0x08:
        if (localCount < usage.size()) usage[localCount++] = localUsage;
        break;
      case 0x18: firstUsage = localUsage; break;
      case 0x28: lastUsage = localUsage; break;
      case 0xA0:
        ++depth;
        if (!gamepadDepth && value == 1 && localCount && (usage[0] == 0x10004 || usage[0] == 0x10005)) {
          gamepadDepth = depth;
        }
        break;
      case 0xC0:
        if (!depth) return false;
        if (depth == gamepadDepth) gamepadDepth = 0;
        --depth;
        break;
      case 0x80: {
        if (!global.size || global.size > 32 || global.count > kMaxReportBits / global.size) return false;
        unsigned index = 0;
        while (index < count_ && reports_[index].id != global.id) ++index;
        if (index == count_) {
          if (count_ == reports_.size()) return false;
          reports_[count_++].id = global.id;
        }
        auto &report = reports_[index];
        const unsigned total = global.count * global.size;
        if (total > kMaxReportBits - report.bits) return false;
        const bool xboxGuide = mapping == Mapping::XboxBle && depth && !gamepadDepth;
        if ((gamepadDepth || xboxGuide) && (value & 3) == 2) {
          for (unsigned i = 0; i < global.count; ++i) {
            const uint32_t id = i < localCount ? usage[i] :
                                firstUsage && lastUsage >= firstUsage && i <= lastUsage - firstUsage ? firstUsage + i : 0;
            // Some Xbox firmware puts Guide in a separate Consumer control report.
            if (xboxGuide && id != 0xC0223) continue;
            const Field field{report.bits + i * global.size, global.size, global.minimum, global.maximum};
            switch (id) {
              case 0x10030: report.x = field; report.mapped = true; break;
              case 0x10031: report.y = field; report.mapped = true; break;
              case 0x10039: report.hat = field; report.mapped = true; break;
              default:
                for (unsigned button = 0; button < report.buttons.size(); ++button) {
                  if (id == buttonUsages[button] ||
                      (mapping == Mapping::XboxBle && button == 6 && id == 0xC0223)) {
                    report.buttons[button] = field;
                    report.mapped = true;
                  }
                }
                break;
            }
          }
        }
        usable |= report.mapped;
        report.bits += total;
        break;
      }
      default: break;
    }
    if ((prefix & 0x0C) == 0) {
      localCount = 0;
      firstUsage = lastUsage = 0;
    }
  }
  return usable && depth == 0;
}

int32_t HidGamepad::readField(const uint8_t *report, const Field &field) {
  uint32_t value = 0;
  for (unsigned i = 0; i < field.bits; ++i) {
    const unsigned bit = field.offset + i;
    value |= uint32_t((report[bit / 8] >> (bit % 8)) & 1) << i;
  }
  return field.minimum < 0 ? signExtend(value, field.bits) : int32_t(value);
}

uint16_t HidGamepad::axisDirection(const uint8_t *bytes, const Field &axis, uint16_t low, uint16_t high) {
  if (!axis.bits || axis.maximum <= axis.minimum) return 0;
  const int64_t span = int64_t(axis.maximum) - axis.minimum;
  const int64_t position = int64_t(readField(bytes, axis)) - axis.minimum;
  if (position * 4 < span) return low;
  if (position * 4 > span * 3) return high;
  return 0;
}

bool HidGamepad::hasInputs(uint8_t id) const {
  for (unsigned i = 0; i < count_; ++i) {
    if (reports_[i].id == id && reports_[i].mapped) return true;
  }
  return false;
}

uint16_t HidGamepad::usbReport(const uint8_t *bytes, size_t length) {
  if (usesIds_ && length) return report(bytes + 1, length - 1, *bytes);
  return report(bytes, length, 0);
}

// Each report ID may carry a different subset of the controller's inputs.
uint16_t HidGamepad::report(const uint8_t *bytes, size_t length, uint8_t id) {
  uint16_t all = 0;
  for (unsigned i = 0; i < count_; ++i) {
    auto &report = reports_[i];
    if (id == report.id && (report.bits + 7) / 8 <= length) {
      uint16_t keys = axisDirection(bytes, report.x, GB_PAD_LEFT, GB_PAD_RIGHT) |
                      axisDirection(bytes, report.y, GB_PAD_UP, GB_PAD_DOWN);
      if (report.hat.bits) {
        constexpr uint16_t direction[] = {GB_PAD_UP, GB_PAD_UP | GB_PAD_RIGHT, GB_PAD_RIGHT,
          GB_PAD_RIGHT | GB_PAD_DOWN, GB_PAD_DOWN, GB_PAD_DOWN | GB_PAD_LEFT,
          GB_PAD_LEFT, GB_PAD_LEFT | GB_PAD_UP};
        const int64_t position = int64_t(readField(bytes, report.hat)) - report.hat.minimum;
        if (position >= 0 && position < int(std::size(direction))) keys |= direction[position];
      }
      for (unsigned button = 0; button < report.buttons.size(); ++button) {
        const auto &field = report.buttons[button];
        if (field.bits && readField(bytes, field)) keys |= kPadButtons[button];
      }
      report.held = keys;
    }
    all |= report.held;
  }
  if ((all & (GB_PAD_SELECT | GB_PAD_START)) == (GB_PAD_SELECT | GB_PAD_START)) {
    all = (all & ~(GB_PAD_SELECT | GB_PAD_START)) | kMenu;
  }
  return all;
}
