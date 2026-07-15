#include "ConfigStore.h"

#include <ArduinoJson.h>

namespace chc {

namespace {
constexpr const char* kNamespace = "home-ctl";
constexpr const char* kLegacyConfigKey = "config";
constexpr const char* kSlotKeys[] = {"config_a", "config_b"};
constexpr const char* kAcStateKey = "ac_state";

uint32_t checksum(const String& value) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= static_cast<uint8_t>(value[i]);
    hash *= 16777619U;
  }
  return hash;
}

String encodeConfig(const AppConfig& config) {
  JsonDocument doc;
  doc["schema"] = kConfigSchemaVersion;
  doc["wifi"]["ssid"] = config.wifi.ssid;
  doc["wifi"]["password"] = config.wifi.password;
  doc["tapo"]["username"] = config.tapo.username;
  doc["tapo"]["password"] = config.tapo.password;
  doc["dyson"]["serial"] = config.dyson.serial;
  doc["dyson"]["product_type"] = config.dyson.productType;
  doc["dyson"]["credential"] = config.dyson.credential;
  String raw;
  serializeJson(doc, raw);
  return raw;
}

bool decodeConfig(const String& raw, AppConfig& config, uint32_t& foundVersion) {
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  foundVersion = doc["schema"] | 0;
  if (foundVersion != 1 && foundVersion != kConfigSchemaVersion) return false;
  config.schemaVersion = kConfigSchemaVersion;
  config.wifi.ssid = doc["wifi"]["ssid"] | "";
  config.wifi.password = doc["wifi"]["password"] | "";
  config.tapo.username = doc["tapo"]["username"] | "";
  config.tapo.password = doc["tapo"]["password"] | "";
  config.dyson.serial = doc["dyson"]["serial"] | "";
  config.dyson.productType = doc["dyson"]["product_type"] | "438K";
  config.dyson.credential = doc["dyson"]["credential"] | "";
  return config.structurallyValid();
}

struct SlotValue {
  bool valid{false};
  uint32_t revision{0};
  String payload;
};

SlotValue decodeSlot(const String& raw) {
  SlotValue slot;
  if (raw.isEmpty()) return slot;
  JsonDocument envelope;
  if (deserializeJson(envelope, raw) != DeserializationError::Ok || (envelope["storage"] | 0) != 1) return slot;
  slot.revision = envelope["revision"] | 0;
  slot.payload = envelope["payload"] | "";
  const uint32_t expected = envelope["checksum"] | 0;
  AppConfig test;
  uint32_t version = 0;
  slot.valid = !slot.payload.isEmpty() && expected == checksum(slot.payload) && decodeConfig(slot.payload, test, version);
  return slot;
}
}  // namespace

bool ConfigStore::load(AppConfig& config) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  const SlotValue slots[] = {decodeSlot(prefs.getString(kSlotKeys[0], "")),
                             decodeSlot(prefs.getString(kSlotKeys[1], ""))};
  const String legacy = prefs.getString(kLegacyConfigKey, "");
  prefs.end();

  int selected = -1;
  if (slots[0].valid) selected = 0;
  if (slots[1].valid && (selected < 0 || slots[1].revision > slots[selected].revision)) selected = 1;
  uint32_t version = 0;
  if (selected >= 0 && decodeConfig(slots[selected].payload, config, version)) {
    revision_ = slots[selected].revision;
    activeSlot_ = selected;
    return true;
  }
  if (legacy.isEmpty()) return false;
  if (!decodeConfig(legacy, config, version)) {
    legacyVersion_ = version;
    return false;
  }
  revision_ = 0;
  activeSlot_ = -1;
  return true;
}

bool ConfigStore::save(const AppConfig& config) {
  if (!config.structurallyValid()) return false;
  const String payload = encodeConfig(config);
  JsonDocument envelope;
  envelope["storage"] = 1;
  envelope["revision"] = revision_ + 1;
  envelope["checksum"] = checksum(payload);
  envelope["payload"] = payload;
  String raw;
  serializeJson(envelope, raw);

  const int target = activeSlot_ == 0 ? 1 : 0;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const size_t written = prefs.putString(kSlotKeys[target], raw);
  const String verified = prefs.getString(kSlotKeys[target], "");
  prefs.end();
  if (written != raw.length() || verified != raw || !decodeSlot(verified).valid) return false;
  activeSlot_ = target;
  ++revision_;
  return true;
}

bool ConfigStore::loadAcState(AcState& state) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  const String raw = prefs.getString(kAcStateKey, "");
  prefs.end();
  JsonDocument doc;
  if (raw.isEmpty() || deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  const int temperature = doc["temperature"] | 0;
  const int mode = doc["mode"] | -1;
  const int fan = doc["fan"] | -1;
  if (temperature < 17 || temperature > 30 || mode < 0 || mode > static_cast<int>(AcMode::Fan) ||
      fan < 0 || fan > static_cast<int>(FanSpeed::High)) return false;
  state.power = doc["power"] | false;
  state.temperature = temperature;
  state.mode = static_cast<AcMode>(mode);
  state.fan = static_cast<FanSpeed>(fan);
  state.swingVertical = doc["swing"] | false;
  state.sleep = doc["sleep"] | false;
  state.turbo = doc["turbo"] | false;
  state.eco = doc["eco"] | false;
  state.clean = doc["clean"] | false;
  state.led = doc["led"] | true;
  state.onTimerMinutes = doc["on_timer"] | 0;
  state.offTimerMinutes = doc["off_timer"] | 0;
  state.lastSentMs = 0;
  return true;
}

bool ConfigStore::saveAcState(const AcState& state) {
  JsonDocument doc;
  doc["power"] = state.power;
  doc["temperature"] = state.temperature;
  doc["mode"] = static_cast<uint8_t>(state.mode);
  doc["fan"] = static_cast<uint8_t>(state.fan);
  doc["swing"] = state.swingVertical;
  doc["sleep"] = state.sleep;
  doc["turbo"] = state.turbo;
  doc["eco"] = state.eco;
  doc["clean"] = state.clean;
  doc["led"] = state.led;
  doc["on_timer"] = state.onTimerMinutes;
  doc["off_timer"] = state.offTimerMinutes;
  String raw;
  serializeJson(doc, raw);
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const size_t written = prefs.putString(kAcStateKey, raw);
  prefs.end();
  return written == raw.length();
}

}  // namespace chc
