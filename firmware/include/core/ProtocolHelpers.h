#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Models.h"

namespace chc {

inline std::string dysonCommandTopic(const std::string& productType,
                                     const std::string& serial) {
  return productType + "/" + serial + "/command";
}

inline std::string dysonStatusTopic(const std::string& productType,
                                    const std::string& serial) {
  return productType + "/" + serial + "/status/current";
}

inline std::string dysonFourDigits(int value) {
  value = std::max(0, std::min(value, 9999));
  char result[5]{};
  std::snprintf(result, sizeof(result), "%04d", value);
  return result;
}

inline uint32_t nextReconnectDelay(uint32_t currentMs) {
  if (currentMs < 1000) currentMs = 1000;
  return std::min<uint32_t>(currentMs * 2, 60000);
}

inline std::vector<uint8_t> klapSequenceBytes(uint32_t sequence) {
  return {static_cast<uint8_t>(sequence >> 24),
          static_cast<uint8_t>(sequence >> 16),
          static_cast<uint8_t>(sequence >> 8),
          static_cast<uint8_t>(sequence)};
}

inline std::vector<uint8_t> pkcs7Pad(const std::vector<uint8_t>& input,
                                    size_t blockSize = 16) {
  if (blockSize == 0 || blockSize > 255) return {};
  const uint8_t padding = static_cast<uint8_t>(blockSize - input.size() % blockSize);
  std::vector<uint8_t> output(input);
  output.insert(output.end(), padding, padding);
  return output;
}

inline bool validPkcs7Padding(const uint8_t* data, size_t length, size_t blockSize = 16) {
  if (!data || !length || !blockSize || blockSize > 255) return false;
  const uint8_t padding = data[length - 1];
  if (!padding || padding > blockSize || padding > length) return false;
  uint8_t difference = 0;
  for (size_t i = length - padding; i < length; ++i) difference |= data[i] ^ padding;
  return difference == 0;
}

inline bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t length) {
  if (!left || !right) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

inline bool isMideaOneShotToggle(CommandKind kind) {
  return kind == CommandKind::ToggleSwing || kind == CommandKind::ToggleTurbo ||
         kind == CommandKind::ToggleEco || kind == CommandKind::ToggleClean ||
         kind == CommandKind::ToggleLed;
}

inline uint8_t reverseByte(uint8_t value) {
  uint8_t reversed = 0;
  for (uint8_t bit = 0; bit < 8; ++bit) reversed = static_cast<uint8_t>((reversed << 1) | ((value >> bit) & 1U));
  return reversed;
}

inline uint8_t mideaChecksum(uint64_t state) {
  uint8_t sum = 0;
  uint64_t bytes = state;
  for (uint8_t index = 0; index < 5; ++index) {
    bytes >>= 8;
    sum = static_cast<uint8_t>(sum + reverseByte(static_cast<uint8_t>(bytes)));
  }
  return reverseByte(static_cast<uint8_t>(0U - sum));
}

inline bool mideaChecksumValid(uint64_t state) {
  return static_cast<uint8_t>(state) == mideaChecksum(state);
}

// Pinned IRremoteESP8266 2.9.0 vector family: power on, cool, low fan,
// native Celsius. Used to catch regressions that would re-enable Fahrenheit.
inline uint64_t mideaCoolLowCelsiusVector(uint8_t temperature) {
  if (temperature < 17) temperature = 17;
  if (temperature > 30) temperature = 30;
  uint64_t state = 0xA18840FFFF00ULL | (static_cast<uint64_t>(temperature - 17) << 24);
  return state | mideaChecksum(state);
}

}  // namespace chc
