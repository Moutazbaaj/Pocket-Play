#pragma once

#include <atomic>
#include <stdint.h>

struct ControllerInput {
  uint16_t held;
  uint16_t pressed;
};

// Transport callbacks publish a snapshot and latch short taps until the launcher consumes them.
class ControllerButtons {
 public:
  void publish(uint16_t held) {
    uint32_t previous = state_.load(std::memory_order_relaxed);
    uint32_t next;
    do {
      const uint16_t pressed = held & ~uint16_t(previous);
      next = (previous & 0xFFFF0000u) | (uint32_t(pressed) << 16) | held;
    } while (!state_.compare_exchange_weak(previous, next, std::memory_order_release,
                                            std::memory_order_relaxed));
  }
  void clear() { state_.store(0, std::memory_order_release); }
  ControllerInput take() {
    const uint32_t value = state_.fetch_and(0xFFFFu, std::memory_order_acq_rel);
    return {uint16_t(value), uint16_t(value >> 16)};
  }

 private:
  std::atomic<uint32_t> state_{0};
};
