#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

// Smooth reconstructed cell voltage for the UI. The caller owns ADC configuration and sampling.
class BatteryGauge {
public:
  static constexpr int unavailable = -1;
  static constexpr int lowPercent = 20;
  static constexpr uint32_t sampleIntervalMs = 250;

  // Pass the current millis() value.
  bool needsSample(uint32_t now) const;
  // Pass cell millivolts or zero if unavailable. Returns true when the displayed percentage changes.
  bool update(uint32_t now, uint32_t millivolts);
  // Return the display estimate from 0 to 100 or unavailable.
  int percent() const { return displayedPercent; }
  // Return the latest plausible reading before smoothing, or zero.
  uint32_t latestMillivolts() const { return latestVoltage; }

private:
  static constexpr size_t windowSize = 5;
  // Ring of recent valid cell readings used by the median filter.
  std::array<uint16_t, windowSize> samples{};
  size_t nextSample = 0;
  size_t sampleCount = 0;
  uint8_t lowSamples = 0;
  uint8_t invalidSamples = 0;
  uint32_t sampleAt = 0;
  uint32_t displayAt = 0;
  uint32_t latestVoltage = 0;
  // Filtered cell millivolts with eight fractional bits.
  uint32_t filteredQ8 = 0;
  int displayedPercent = unavailable;
  bool hasSample = false;
};
