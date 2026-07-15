#pragma once

#include <Preferences.h>

#include "core/Config.h"
#include "core/Models.h"

namespace chc {

class ConfigStore {
 public:
  bool load(AppConfig& config);
  bool save(const AppConfig& config);
  bool loadAcState(AcState& state);
  bool saveAcState(const AcState& state);
  bool hasLegacyConfig() const { return legacyVersion_ != 0; }
  uint32_t legacyVersion() const { return legacyVersion_; }

 private:
  uint32_t legacyVersion_{0};
  uint32_t revision_{0};
  int8_t activeSlot_{-1};
};

}  // namespace chc
