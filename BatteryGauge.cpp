#include "BatteryGauge.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace {
constexpr uint16_t kLowMillivolts = 3600;
constexpr uint8_t kConfirmSamples = 4;
constexpr uint32_t kHistoryTimeoutMs = 2000;
constexpr uint32_t kDisplayIntervalMs = 1000;
constexpr int kDisplayHysteresis = 2;
constexpr uint32_t kFilterDivisor = 80;  // About 20 seconds at 250 ms/sample.

// Map cell millivolts to an approximate percentage using piecewise linear interpolation.
int estimatePercent(uint32_t millivolts) {
  struct Level {
    uint16_t millivolts;
    uint8_t percent;
  };
  // Approximate 4.2 V Li-ion cell discharge curve under load.
  constexpr Level levels[] = {
    {3300, 0}, {3500, 10}, {kLowMillivolts, BatteryGauge::lowPercent}, {3700, 40},
    {3800, 60}, {3950, 80}, {4200, 100},
  };
  if (millivolts <= levels[0].millivolts) return 0;
  for (size_t i = 1; i < std::size(levels); ++i) {
    const auto &lower = levels[i - 1];
    const auto &upper = levels[i];
    if (millivolts <= upper.millivolts) {
      const unsigned span = upper.millivolts - lower.millivolts;
      const unsigned rise = (millivolts - lower.millivolts) * (upper.percent - lower.percent);
      return lower.percent + (rise + span / 2) / span;
    }
  }
  return 100;
}
}

// Check whether the caller should read the ADC at this millisecond timestamp.
bool BatteryGauge::needsSample(uint32_t now) const {
  return !hasSample || now - sampleAt >= sampleIntervalMs;
}

// Reject isolated invalid readings and smooth valid samples before updating the display estimate.
// A confirmed low-voltage run bypasses the slow filter so sustained dips appear promptly.
bool BatteryGauge::update(uint32_t now, uint32_t millivolts) {
  const int previous = displayedPercent;
  if (hasSample && now - sampleAt > kHistoryTimeoutMs) {
    // Menus may resume after a game or sleep. Start from the fresh reading.
    *this = BatteryGauge{};
  }
  hasSample = true;
  sampleAt = now;

  if (millivolts < 2800 || millivolts > 4300) {
    latestVoltage = 0;
    lowSamples = 0;
    if (invalidSamples < kConfirmSamples) ++invalidSamples;
    if (invalidSamples == kConfirmSamples) {
      sampleCount = nextSample = filteredQ8 = 0;
      displayedPercent = unavailable;
    }
    return previous != displayedPercent;
  }
  invalidSamples = 0;
  latestVoltage = millivolts;
  if (millivolts > kLowMillivolts) {
    lowSamples = 0;
  } else if (lowSamples < kConfirmSamples) {
    ++lowSamples;
  }

  samples[nextSample] = uint16_t(millivolts);
  nextSample = (nextSample + 1) % windowSize;
  sampleCount = std::min(sampleCount + 1, windowSize);
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.begin() + sampleCount);
  const uint32_t medianQ8 = uint32_t(sorted[sampleCount / 2]) << 8;

  // Median rejects brief beat-related dips. Fixed-point smoothing keeps
  // sustained changes moving even when the per-sample change is below 1 mV.
  if (!filteredQ8) {
    filteredQ8 = medianQ8;
  } else {
    filteredQ8 = (filteredQ8 * (kFilterDivisor - 1) + medianQ8 + kFilterDivisor / 2) / kFilterDivisor;
  }
  if (lowSamples == kConfirmSamples) {
    // A verified low voltage should reach the warning without the slow filter.
    filteredQ8 = std::min(filteredQ8, medianQ8);
  }

  const int estimate = estimatePercent((filteredQ8 + 128) >> 8);
  const bool lowCrossing = (estimate <= lowPercent) != (displayedPercent <= lowPercent);
  const bool updateDue = now - displayAt >= kDisplayIntervalMs;
  const bool visibleChange = std::abs(estimate - displayedPercent) >= kDisplayHysteresis ||
                             estimate == 0 || estimate == 100;
  if (estimate != displayedPercent &&
      (displayedPercent == unavailable || lowCrossing || (updateDue && visibleChange))) {
    displayedPercent = estimate;
    displayAt = now;
  }
  return previous != displayedPercent;
}
