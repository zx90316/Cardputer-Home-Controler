#pragma once

#include <WebServer.h>

#include "ConfigStore.h"

namespace chc {

enum class SetupValidationStage : uint8_t { None, Wifi, Tapo, Dyson };

struct SetupValidationResult {
  bool ok{false};
  SetupValidationStage stage{SetupValidationStage::None};
  char detail[96]{};
};

class SetupPortal {
 public:
  using Validator = SetupValidationResult (*)(const AppConfig& candidate);

  bool begin(ConfigStore& store, AppConfig& config, Validator validator);
  void tick(uint32_t nowMs);
  bool active() const { return active_; }
  const String& apPassword() const { return apPassword_; }

 private:
  void handleRoot();
  void handleSave();
  String field(const char* name, bool trim = true);
  void noStore();

  WebServer server_{80};
  ConfigStore* store_{nullptr};
  AppConfig* config_{nullptr};
  Validator validator_{nullptr};
  String apPassword_;
  uint32_t startedMs_{0};
  bool active_{false};
};

}  // namespace chc
