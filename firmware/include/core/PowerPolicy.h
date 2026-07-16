#pragma once

#include <cmath>
#include <cstdint>

#include "TimeUtils.h"

namespace chc {

enum class ScreenPowerMode : uint8_t { Awake, Dimmed, Sleeping };

inline ScreenPowerMode screenPowerMode(uint32_t nowMs, uint32_t lastActivityMs,
                                       uint32_t dimAfterMs = 30000,
                                       uint32_t sleepAfterMs = 120000) {
  const uint32_t idleMs = elapsedSince(nowMs, lastActivityMs);
  if (idleMs >= sleepAfterMs) return ScreenPowerMode::Sleeping;
  if (idleMs >= dimAfterMs) return ScreenPowerMode::Dimmed;
  return ScreenPowerMode::Awake;
}

inline bool motionDetected(float ax, float ay, float az,
                           float previousAx, float previousAy, float previousAz,
                           float gx, float gy, float gz,
                           float accelDeltaThreshold = 0.12F,
                           float gyroThreshold = 15.0F) {
  const float accelDelta = std::fabs(ax - previousAx) + std::fabs(ay - previousAy) +
                           std::fabs(az - previousAz);
  const float gyroActivity = std::fabs(gx) + std::fabs(gy) + std::fabs(gz);
  return accelDelta >= accelDeltaThreshold || gyroActivity >= gyroThreshold;
}

}  // namespace chc
