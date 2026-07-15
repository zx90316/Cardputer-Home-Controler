#pragma once

#include <cstdint>

namespace chc {

inline bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

inline uint32_t elapsedSince(uint32_t nowMs, uint32_t startedMs) {
  return nowMs - startedMs;
}

}  // namespace chc
