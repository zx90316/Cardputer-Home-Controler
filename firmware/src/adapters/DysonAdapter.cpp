#include "adapters/DysonAdapter.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <time.h>

#include "core/ProtocolHelpers.h"
#include "core/TimeUtils.h"

namespace chc {

DysonAdapter* DysonAdapter::instance_ = nullptr;

DysonAdapter::DysonAdapter() : mqtt_(network_) { instance_ = this; }

bool DysonAdapter::begin(const AppConfig& config) {
  if (mqtt_.connected()) mqtt_.disconnect();
  config_ = config.dyson;
  if (config_.serial.empty() || config_.credential.empty()) return false;
  mqtt_.setBufferSize(3072);
  mqtt_.setKeepAlive(60);
  mqtt_.setCallback(mqttCallback);
  connection_ = ConnectionState::Connecting;
  runtimeHost_ = "";
  nextDiscoveryMs_ = 0;
  nextReconnectMs_ = 0;
  health_.connection = connection_;
  health_.totalTargets = 1;
  return discoverHost();
}

AdapterHealth DysonAdapter::health() const {
  AdapterHealth value = health_;
  value.connection = connection_;
  value.onlineTargets = connection_ == ConnectionState::Online ? 1 : 0;
  value.totalTargets = 1;
  value.nextRetryMs = nextReconnectMs_;
  return value;
}

void DysonAdapter::setError(const char* message, uint32_t nowMs) {
  health_.lastErrorMs = nowMs;
  strncpy(health_.lastError, message ? message : "unknown", sizeof(health_.lastError) - 1);
  health_.lastError[sizeof(health_.lastError) - 1] = '\0';
}

bool DysonAdapter::discoverHost() {
  static bool mdnsStarted = false;
  if (!mdnsStarted) mdnsStarted = MDNS.begin("cardputer-home");
  if (!mdnsStarted) {
    connection_ = ConnectionState::Offline;
    setError("mDNS start failed", millis());
    return false;
  }
  const int count = MDNS.queryService("dyson_mqtt", "tcp");
  int fallback = count == 1 ? 0 : -1;
  int selected = -1;
  String wanted(config_.serial.c_str());
  wanted.toLowerCase();
  for (int i = 0; i < count; ++i) {
    String searchable = MDNS.hostname(i);
    for (int t = 0; t < MDNS.numTxt(i); ++t) {
      searchable += " "; searchable += MDNS.txtKey(i, t);
      searchable += " "; searchable += MDNS.txt(i, t);
    }
    searchable.toLowerCase();
    if (searchable.indexOf(wanted) >= 0) { selected = i; break; }
  }
  if (selected < 0) selected = fallback;
  if (selected < 0 || MDNS.IP(selected) == IPAddress()) {
    runtimeHost_ = "";
    connection_ = ConnectionState::Offline;
    setError(count > 1 ? "TP09 serial not matched" : "TP09 mDNS not found", millis());
    return false;
  }
  runtimeHost_ = MDNS.IP(selected).toString();
  mqtt_.setServer(runtimeHost_.c_str(), MDNS.port(selected) ? MDNS.port(selected) : 1883);
  connection_ = ConnectionState::Connecting;
  nextDiscoveryMs_ = millis() + 60000;
  return true;
}

String DysonAdapter::commandTopic() const {
  return String(dysonCommandTopic(config_.productType, config_.serial).c_str());
}
String DysonAdapter::statusTopic() const {
  return String(dysonStatusTopic(config_.productType, config_.serial).c_str());
}

String DysonAdapter::mqttTimestamp() {
  time_t now = time(nullptr);
  struct tm value{};
  gmtime_r(&now, &value);
  char text[24];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &value);
  return text;
}

bool DysonAdapter::reconnect(uint32_t nowMs) {
  if (mqtt_.connected()) return true;
  if (nextReconnectMs_ && !deadlineReached(nowMs, nextReconnectMs_)) return false;
  if (runtimeHost_.isEmpty() || !nextDiscoveryMs_ || deadlineReached(nowMs, nextDiscoveryMs_)) {
    if (!discoverHost()) {
      nextReconnectMs_ = nowMs + 5000;
      return false;
    }
  }
  connection_ = ConnectionState::Connecting;
  const String clientId = "cardputer-" + String(static_cast<uint32_t>(esp_random()), HEX);
  if (mqtt_.connect(clientId.c_str(), config_.serial.c_str(), config_.credential.c_str())) {
    mqtt_.subscribe(statusTopic().c_str(), 1);
    reconnectDelayMs_ = 1000;
    nextReconnectMs_ = 0;
    connection_ = ConnectionState::Online;
    publishRequest("REQUEST-CURRENT-STATE");
    publishRequest("REQUEST-PRODUCT-ENVIRONMENT-CURRENT-SENSOR-DATA");
    return true;
  }
  connection_ = mqtt_.state() == MQTT_CONNECT_BAD_CREDENTIALS ? ConnectionState::AuthFailed : ConnectionState::Offline;
  setError(connection_ == ConnectionState::AuthFailed ? "MQTT authentication failed" : "MQTT connection failed", nowMs);
  if (connection_ == ConnectionState::Offline && reconnectDelayMs_ >= 30000) nextDiscoveryMs_ = 0;
  nextReconnectMs_ = nowMs + reconnectDelayMs_;
  reconnectDelayMs_ = nextReconnectDelay(reconnectDelayMs_);
  return false;
}

void DysonAdapter::tick(uint32_t nowMs) {
  if (mqtt_.connected() && state_.lastReportMs && elapsedSince(nowMs, state_.lastReportMs) > 90000) {
    mqtt_.disconnect();
    connection_ = ConnectionState::Offline;
    runtimeHost_ = "";
    nextDiscoveryMs_ = 0;
    nextReconnectMs_ = nowMs + 1000;
    setError("MQTT state stale", nowMs);
  }
  if (!reconnect(nowMs)) {
    resolvePending(nowMs);
    return;
  }
  if (!mqtt_.loop()) {
    connection_ = ConnectionState::Offline;
    setError("MQTT loop disconnected", nowMs);
  }
  if (!nextPollMs_ || deadlineReached(nowMs, nextPollMs_)) {
    publishRequest("REQUEST-CURRENT-STATE");
    publishRequest("REQUEST-PRODUCT-ENVIRONMENT-CURRENT-SENSOR-DATA");
    nextPollMs_ = nowMs + 30000;
  }
  resolvePending(nowMs);
}

bool DysonAdapter::publishRequest(const char* message) {
  JsonDocument doc;
  doc["msg"] = message;
  doc["time"] = mqttTimestamp();
  String payload;
  serializeJson(doc, payload);
  return mqtt_.publish(commandTopic().c_str(), payload.c_str(), false);
}

bool DysonAdapter::publishStateSet(JsonObjectConst data) {
  JsonDocument doc;
  doc["msg"] = "STATE-SET";
  doc["time"] = mqttTimestamp();
  doc["mode-reason"] = "LAPP";
  doc["data"].set(data);
  String payload;
  serializeJson(doc, payload);
  return mqtt_.publish(commandTopic().c_str(), payload.c_str(), false);
}

DeviceEvent DysonAdapter::execute(const DeviceCommand& command) {
  if (!mqtt_.connected()) return makeEvent(DeviceId::Dyson, command.id, EventResult::Offline, "mqtt offline");
  JsonDocument values;
  JsonObject data = values.to<JsonObject>();
  DeviceCommand expected = command;
  switch (command.kind) {
    case CommandKind::TogglePower:
      expected.kind = CommandKind::SetPower;
      expected.value1 = !state_.power;
      data["fpwr"] = expected.value1 ? "ON" : "OFF";
      break;
    case CommandKind::SetPower: data["fpwr"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetFanSpeed:
      if (command.value1 < 1 || command.value1 > 10) return makeEvent(DeviceId::Dyson, command.id, EventResult::Unsupported, "speed 1..10");
      data["fpwr"] = "ON"; data["auto"] = "OFF";
      data["fnsp"] = dysonFourDigits(command.value1);
      break;
    case CommandKind::SetDysonAuto: data["auto"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetOscillation: data["oson"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetOscillationAngles:
      if (command.value1 < 5 || command.value2 > 355 || command.value1 + 30 > command.value2)
        return makeEvent(DeviceId::Dyson, command.id, EventResult::Unsupported, "bad angles");
      data["fpwr"] = "ON"; data["oson"] = "ON"; data["ancp"] = "CUST";
      data["osal"] = dysonFourDigits(command.value1);
      data["osau"] = dysonFourDigits(command.value2);
      break;
    case CommandKind::SetAirflowFront: data["fdir"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetNightMode: data["nmod"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetContinuousMonitoring: data["rhtm"] = command.value1 ? "ON" : "OFF"; break;
    case CommandKind::SetSleepTimer:
      if (command.value1 < 0 || command.value1 > 540)
        return makeEvent(DeviceId::Dyson, command.id, EventResult::Unsupported, "timer 0..540");
      data["sltm"] = command.value1 == 0 ? "OFF" : dysonFourDigits(command.value1);
      break;
    case CommandKind::Refresh:
      if (!publishRequest("REQUEST-CURRENT-STATE") ||
          !publishRequest("REQUEST-PRODUCT-ENVIRONMENT-CURRENT-SENSOR-DATA"))
        return makeEvent(DeviceId::Dyson, command.id, EventResult::Offline, "refresh publish failed");
      if (!rememberPending(expected, millis()))
        return makeEvent(DeviceId::Dyson, command.id, EventResult::TimedOut, "pending table full");
      return makeEvent(DeviceId::Dyson, command.id, EventResult::Pending, "refresh requested");
    default: return makeEvent(DeviceId::Dyson, command.id, EventResult::Unsupported, "unsupported command");
  }
  if (!publishStateSet(data)) {
    setError("MQTT publish failed", millis());
    return makeEvent(DeviceId::Dyson, command.id, EventResult::Offline, "MQTT publish failed");
  }
  if (!rememberPending(expected, millis()))
    return makeEvent(DeviceId::Dyson, command.id, EventResult::TimedOut, "pending table full");
  return makeEvent(DeviceId::Dyson, command.id, EventResult::Pending, "awaiting MQTT state");
}

bool DysonAdapter::rememberPending(const DeviceCommand& command, uint32_t nowMs) {
  for (auto& item : pending_) {
    if (!item.active) {
      item.active = true;
      item.command = command;
      item.issuedMs = nowMs;
      item.deadlineMs = nowMs + 3000;
      return true;
    }
  }
  return false;
}

bool DysonAdapter::commandMatches(const DeviceCommand& command) const {
  switch (command.kind) {
    case CommandKind::SetPower: return state_.power == (command.value1 != 0);
    case CommandKind::SetFanSpeed: return state_.power && !state_.autoMode && state_.speed == command.value1;
    case CommandKind::SetDysonAuto: return state_.autoMode == (command.value1 != 0);
    case CommandKind::SetOscillation: return state_.oscillation == (command.value1 != 0);
    case CommandKind::SetOscillationAngles:
      return state_.oscillation && state_.angleLow == command.value1 && state_.angleHigh == command.value2;
    case CommandKind::SetAirflowFront: return state_.frontAirflow == (command.value1 != 0);
    case CommandKind::SetNightMode: return state_.nightMode == (command.value1 != 0);
    case CommandKind::SetContinuousMonitoring: return state_.continuousMonitoring == (command.value1 != 0);
    case CommandKind::SetSleepTimer:
      return command.value1 == 0 ? state_.sleepTimerMinutes <= 0 : state_.sleepTimerMinutes == command.value1;
    case CommandKind::Refresh: return state_.lastReportMs != 0;
    default: return false;
  }
}

void DysonAdapter::resolvePending(uint32_t nowMs) {
  for (auto& item : pending_) {
    if (!item.active) continue;
    if (state_.lastReportMs && deadlineReached(state_.lastReportMs, item.issuedMs) && commandMatches(item.command)) {
      events_.push(makeEvent(DeviceId::Dyson, item.command.id, EventResult::Succeeded, "MQTT state confirmed",
                             ConfirmationKind::MqttState, 1, 1));
      item.active = false;
    } else if (deadlineReached(nowMs, item.deadlineMs)) {
      events_.push(makeEvent(DeviceId::Dyson, item.command.id, EventResult::TimedOut, "MQTT state timeout",
                             ConfirmationKind::MqttState, 0, 1));
      setError("MQTT command confirmation timeout", nowMs);
      item.active = false;
    }
  }
}

void DysonAdapter::mqttCallback(char*, uint8_t* payload, unsigned int length) {
  if (instance_) instance_->onMessage(payload, length);
}

const char* DysonAdapter::fieldText(JsonVariantConst value, const char* fallback) {
  if (value.is<JsonArrayConst>() && value.size() > 1) return value[1] | fallback;
  return value | fallback;
}

int DysonAdapter::fieldInt(JsonVariantConst value, int fallback) {
  const char* text = fieldText(value, nullptr);
  if (!text || strcmp(text, "OFF") == 0 || strcmp(text, "INV") == 0) return fallback;
  return atoi(text);
}

void DysonAdapter::onMessage(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;
  const char* type = doc["msg"] | "";
  if (strcmp(type, "CURRENT-STATE") == 0 || strcmp(type, "STATE-CHANGE") == 0) {
    JsonObjectConst s = doc["product-state"];
    if (!s.isNull()) {
      if (s["fpwr"].is<JsonVariantConst>()) state_.power = strcmp(fieldText(s["fpwr"]), "ON") == 0;
      if (s["auto"].is<JsonVariantConst>()) state_.autoMode = strcmp(fieldText(s["auto"]), "ON") == 0;
      if (s["fnsp"].is<JsonVariantConst>()) state_.speed = static_cast<uint8_t>(fieldInt(s["fnsp"], state_.speed));
      if (s["oson"].is<JsonVariantConst>()) { const char* v = fieldText(s["oson"]); state_.oscillation = strcmp(v, "ON") == 0 || strcmp(v, "OION") == 0; }
      if (s["osal"].is<JsonVariantConst>()) state_.angleLow = fieldInt(s["osal"], state_.angleLow);
      if (s["osau"].is<JsonVariantConst>()) state_.angleHigh = fieldInt(s["osau"], state_.angleHigh);
      if (s["fdir"].is<JsonVariantConst>()) state_.frontAirflow = strcmp(fieldText(s["fdir"]), "ON") == 0;
      if (s["nmod"].is<JsonVariantConst>()) state_.nightMode = strcmp(fieldText(s["nmod"]), "ON") == 0;
      if (s["rhtm"].is<JsonVariantConst>()) state_.continuousMonitoring = strcmp(fieldText(s["rhtm"]), "ON") == 0;
      if (s["sltm"].is<JsonVariantConst>()) state_.sleepTimerMinutes = fieldInt(s["sltm"], -1);
      if (s["cflr"].is<JsonVariantConst>()) state_.carbonFilter = fieldInt(s["cflr"], -1);
      if (s["hflr"].is<JsonVariantConst>()) state_.hepaFilter = fieldInt(s["hflr"], -1);
    }
  } else if (strcmp(type, "ENVIRONMENTAL-CURRENT-SENSOR-DATA") == 0) {
    JsonObjectConst s = doc["data"];
    state_.humidity = fieldInt(s["hact"]);
    state_.temperatureC = (fieldInt(s["tact"], 2731) / 10.0f) - 273.15f;
    state_.pm25 = fieldInt(s["pm25"]); state_.pm10 = fieldInt(s["pm10"]);
    state_.voc = fieldInt(s["va10"]); state_.no2 = fieldInt(s["noxl"]); state_.formaldehyde = fieldInt(s["hcho"]);
  }
  state_.lastReportMs = millis();
  health_.lastSuccessMs = state_.lastReportMs;
  health_.lastError[0] = '\0';
  resolvePending(state_.lastReportMs);
}

}  // namespace chc
