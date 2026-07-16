#include "adapters/TapoAdapter.h"

#include "core/StateReducer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

#include "core/TimeUtils.h"
#include "core/ProtocolHelpers.h"

namespace chc {

namespace {
constexpr uint32_t kSessionLifetimeMs = 23UL * 60UL * 60UL * 1000UL;
constexpr uint16_t kLegacyDiscoveryPort = 9999;
constexpr uint16_t kSmartDiscoveryPort = 20002;
constexpr size_t kMaxResponseBytes = 8192;

const char kDiscoveryPayload[] = R"JSON({"params":{"rsa_key":"-----BEGIN PUBLIC KEY-----\nMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAqcJQQ7bQbbgAQyJjoEYh\n+09VzlGF2JUdaT6wKx1UVpaPH76S+cmeoblufHeXIhU0jFo6rBjYfUuduqXN4dq0\nRaaGmWvbZoqgpqW02nAvZfvV3p3y9W7Lg6M/kvty3QhH5zyJf7FUtbGI1aJPMLan\nHPhlkQo6GFDgMa8yM9oLfBZho81OSpwQK0/gAIROXN6L/U++fO9CEC/MMPlQXe4K\nBd2BbUPf466/dKVJpSAt68hHo8PDeT1DSKsmX9zkJjg62t8UWoBt4Yd8XgLBgJ7i\nMMkvbG3sUKTqZWkhmIHNbdEXTPD+qsRrf4QA0pqPt6oRb8zB+lMuBa+yIr3lP3UL\nUQIDAQAB\n-----END PUBLIC KEY-----\n"}})JSON";

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void writeBe16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8); out[1] = static_cast<uint8_t>(value);
}

void writeBe32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24); out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8); out[3] = static_cast<uint8_t>(value);
}

String extractSessionCookie(const String& header) {
  const int start = header.indexOf("TP_SESSIONID=");
  if (start < 0) return {};
  int end = header.indexOf(';', start);
  if (end < 0) end = header.length();
  return header.substring(start, end);
}
}

std::vector<std::string> TapoAdapter::discoverL530Hosts(uint32_t timeoutMs) {
  std::vector<std::string> hosts;
  if (WiFi.status() != WL_CONNECTED) return hosts;
  WiFiUDP udp;
  const uint16_t localPort = 21000 + static_cast<uint16_t>(esp_random() % 1000);
  if (!udp.begin(localPort)) return hosts;

  const size_t payloadLength = strlen(kDiscoveryPayload);
  std::vector<uint8_t> smartPacket(16 + payloadLength);
  smartPacket[0] = 2; smartPacket[1] = 0;
  writeBe16(smartPacket.data() + 2, 1);
  writeBe16(smartPacket.data() + 4, static_cast<uint16_t>(payloadLength));
  smartPacket[6] = 17; smartPacket[7] = 0;
  writeBe32(smartPacket.data() + 8, esp_random());
  writeBe32(smartPacket.data() + 12, 0x5A6B7C8D);
  memcpy(smartPacket.data() + 16, kDiscoveryPayload, payloadLength);
  writeBe32(smartPacket.data() + 12, crc32(smartPacket.data(), smartPacket.size()));

  constexpr char legacyJson[] = "{\"system\":{\"get_sysinfo\":null}}";
  std::vector<uint8_t> legacyPacket(strlen(legacyJson));
  uint8_t key = 171;
  for (size_t i = 0; i < legacyPacket.size(); ++i) {
    legacyPacket[i] = key ^ static_cast<uint8_t>(legacyJson[i]);
    key = legacyPacket[i];
  }

  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) broadcast[i] = ip[i] | static_cast<uint8_t>(~mask[i]);
  auto sendQueries = [&] {
    udp.beginPacket(broadcast, kSmartDiscoveryPort);
    udp.write(smartPacket.data(), smartPacket.size()); udp.endPacket();
    udp.beginPacket(broadcast, kLegacyDiscoveryPort);
    udp.write(legacyPacket.data(), legacyPacket.size()); udp.endPacket();
  };
  sendQueries();

  const uint32_t started = millis();
  bool repeated = false;
  uint8_t buffer[1600];
  while (millis() - started < timeoutMs) {
    if (!repeated && millis() - started >= 1200) { sendQueries(); repeated = true; }
    const int packetSize = udp.parsePacket();
    if (packetSize <= 0) { delay(10); continue; }
    const size_t length = udp.read(buffer, min<int>(packetSize, static_cast<int>(sizeof(buffer) - 1)));
    bool isL530 = false;
    if (udp.remotePort() == kSmartDiscoveryPort && length > 16) {
      buffer[length] = 0;
      const String body(reinterpret_cast<char*>(buffer + 16));
      isL530 = body.indexOf("L530") >= 0;
    } else if (udp.remotePort() == kLegacyDiscoveryPort) {
      String plain;
      plain.reserve(length);
      uint8_t previous = 171;
      for (size_t i = 0; i < length; ++i) {
        const uint8_t cipher = buffer[i];
        plain += static_cast<char>(previous ^ cipher);
        previous = cipher;
      }
      isL530 = plain.indexOf("L530") >= 0;
    }
    if (isL530) {
      const std::string host = udp.remoteIP().toString().c_str();
      if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) hosts.push_back(host);
    }
  }
  udp.stop();
  return hosts;
}

void TapoAdapter::sha1(const uint8_t* data, size_t length, uint8_t out[20]) {
  mbedtls_sha1_ret(data, length, out);
}

void TapoAdapter::sha256(const uint8_t* data, size_t length, uint8_t out[32]) {
  mbedtls_sha256_ret(data, length, out, 0);
}

void TapoAdapter::putBe32(uint8_t out[4], uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24); out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8); out[3] = static_cast<uint8_t>(value);
}

bool TapoAdapter::begin(const AppConfig& config) {
  baseConfig_ = config;
  config_ = config.tapo;
  if (config_.username.empty() || config_.password.empty()) return false;
  if (groupMode_) {
    children_.clear();
    nextGroupDiscoveryMs_ = 0;
    groupDiscoveryRetryMs_ = 1000;
    return refreshGroup(millis());
  }
  if (runtimeHost_.isEmpty()) return false;
  connection_ = ConnectionState::Connecting;
  if (!handshake()) return false;
  loadPresets();
  return true;
}

AdapterHealth TapoAdapter::health() const {
  AdapterHealth value = health_;
  value.connection = connection_;
  value.onlineTargets = groupMode_ ? state_.onlineDevices : (connection_ == ConnectionState::Online ? 1 : 0);
  value.totalTargets = groupMode_ ? state_.totalDevices : 1;
  value.nextRetryMs = groupMode_ ? nextGroupDiscoveryMs_ : nextPollMs_;
  return value;
}

void TapoAdapter::setError(const char* message, uint32_t nowMs) {
  health_.lastErrorMs = nowMs;
  strncpy(health_.lastError, message ? message : "unknown", sizeof(health_.lastError) - 1);
  health_.lastError[sizeof(health_.lastError) - 1] = '\0';
}

void TapoAdapter::authHashV2() {
  uint8_t usernameHash[20], passwordHash[20], joined[40];
  sha1(reinterpret_cast<const uint8_t*>(config_.username.data()), config_.username.size(), usernameHash);
  sha1(reinterpret_cast<const uint8_t*>(config_.password.data()), config_.password.size(), passwordHash);
  memcpy(joined, usernameHash, 20); memcpy(joined + 20, passwordHash, 20);
  sha256(joined, sizeof(joined), authHash_);
}

bool TapoAdapter::postBinary(const String& url, const uint8_t* data, size_t length,
                             std::vector<uint8_t>& response, bool includeCookie,
                             int& status, String* setCookie) {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(url)) return false;
  const char* headers[] = {"Set-Cookie"};
  http.collectHeaders(headers, 1);
  http.addHeader("Content-Type", "application/octet-stream");
  if (includeCookie && !sessionCookie_.isEmpty()) http.addHeader("Cookie", sessionCookie_);
  status = http.POST(const_cast<uint8_t*>(data), length);
  if (setCookie) *setCookie = http.header("Set-Cookie");
  response.clear();
  if (status > 0) {
    WiFiClient* stream = http.getStreamPtr();
    int remaining = http.getSize();
    if (remaining > static_cast<int>(kMaxResponseBytes)) { http.end(); return false; }
    if (remaining >= 0 && remaining > 0) {
      response.resize(static_cast<size_t>(remaining));
      const size_t received = stream->readBytes(response.data(), response.size());
      response.resize(received);
    } else if (remaining < 0) {
      const uint32_t started = millis();
      while ((stream->connected() || stream->available()) && elapsedSince(millis(), started) < 5000) {
        while (stream->available()) {
          if (response.size() >= kMaxResponseBytes) { http.end(); return false; }
          const size_t chunk = min<size_t>(stream->available(), kMaxResponseBytes - response.size());
          const size_t oldSize = response.size();
          response.resize(oldSize + chunk);
          const size_t received = stream->readBytes(response.data() + oldSize, chunk);
          response.resize(oldSize + received);
        }
        delay(1);
      }
    }
  }
  http.end();
  return status > 0;
}

bool TapoAdapter::handshake() {
  resetSession();
  authHashV2();
  esp_fill_random(localSeed_, sizeof(localSeed_));
  std::vector<uint8_t> response;
  String cookieHeader;
  int status = 0;
  const String base = "http://" + runtimeHost_ + "/app/";
  if (!postBinary(base + "handshake1", localSeed_, sizeof(localSeed_), response, false, status, &cookieHeader) ||
      status != 200 || response.size() != 48) {
    connection_ = ConnectionState::Offline;
    setError("KLAP handshake1 failed", millis());
    return false;
  }
  memcpy(remoteSeed_, response.data(), 16);
  uint8_t challenge[64], expected[32];
  memcpy(challenge, localSeed_, 16); memcpy(challenge + 16, remoteSeed_, 16); memcpy(challenge + 32, authHash_, 32);
  sha256(challenge, sizeof(challenge), expected);
  if (memcmp(expected, response.data() + 16, 32) != 0) {
    connection_ = ConnectionState::AuthFailed;
    setError("KLAP authentication failed", millis());
    return false;
  }
  sessionCookie_ = extractSessionCookie(cookieHeader);
  if (sessionCookie_.isEmpty()) { connection_ = ConnectionState::Offline; setError("KLAP cookie missing", millis()); return false; }

  uint8_t handshake2Input[64], handshake2[32];
  memcpy(handshake2Input, remoteSeed_, 16); memcpy(handshake2Input + 16, localSeed_, 16);
  memcpy(handshake2Input + 32, authHash_, 32);
  sha256(handshake2Input, sizeof(handshake2Input), handshake2);
  if (!postBinary(base + "handshake2", handshake2, sizeof(handshake2), response, true, status) || status != 200) {
    connection_ = status == 403 ? ConnectionState::AuthFailed : ConnectionState::Offline;
    setError(status == 403 ? "KLAP credential rejected" : "KLAP handshake2 failed", millis());
    return false;
  }
  deriveSession();
  sessionExpiresMs_ = millis() + kSessionLifetimeMs;
  connection_ = ConnectionState::Online;
  health_.lastError[0] = '\0';
  return true;
}

void TapoAdapter::deriveSession() {
  uint8_t material[3 + 16 + 16 + 32], digest[32];
  auto derive = [&](const char* label, size_t labelLength, uint8_t* out, size_t count) {
    memcpy(material, label, labelLength); memcpy(material + labelLength, localSeed_, 16);
    memcpy(material + labelLength + 16, remoteSeed_, 16); memcpy(material + labelLength + 32, authHash_, 32);
    sha256(material, labelLength + 64, digest); memcpy(out, digest, count);
  };
  derive("lsk", 3, key_, sizeof(key_));
  derive("iv", 2, digest, sizeof(digest));
  memcpy(ivPrefix_, digest, 12);
  sequence_ = static_cast<int32_t>((static_cast<uint32_t>(digest[28]) << 24) |
                                   (static_cast<uint32_t>(digest[29]) << 16) |
                                   (static_cast<uint32_t>(digest[30]) << 8) | digest[31]);
  derive("ldk", 3, signatureKey_, sizeof(signatureKey_));
}

bool TapoAdapter::encryptedPost(const String& json, String& plaintext) {
  sequence_ = static_cast<int32_t>(static_cast<uint32_t>(sequence_) + 1U);
  uint8_t seqBytes[4]; putBe32(seqBytes, static_cast<uint32_t>(sequence_));
  uint8_t iv[16]; memcpy(iv, ivPrefix_, 12); memcpy(iv + 12, seqBytes, 4);
  const size_t paddedLength = ((json.length() / 16) + 1) * 16;
  std::vector<uint8_t> padded(paddedLength);
  memcpy(padded.data(), json.c_str(), json.length());
  const uint8_t padding = static_cast<uint8_t>(paddedLength - json.length());
  memset(padded.data() + json.length(), padding, padding);

  std::vector<uint8_t> encrypted(paddedLength);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key_, 128);
  uint8_t encIv[16]; memcpy(encIv, iv, 16);
  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLength, encIv, padded.data(), encrypted.data()) != 0) {
    mbedtls_aes_free(&aes); return false;
  }
  mbedtls_aes_free(&aes);

  std::vector<uint8_t> signInput(28 + 4 + encrypted.size());
  memcpy(signInput.data(), signatureKey_, 28); memcpy(signInput.data() + 28, seqBytes, 4);
  memcpy(signInput.data() + 32, encrypted.data(), encrypted.size());
  uint8_t signature[32]; sha256(signInput.data(), signInput.size(), signature);
  std::vector<uint8_t> payload(32 + encrypted.size());
  memcpy(payload.data(), signature, 32); memcpy(payload.data() + 32, encrypted.data(), encrypted.size());

  std::vector<uint8_t> response;
  int status = 0;
  const String url = "http://" + runtimeHost_ + "/app/request?seq=" + String(sequence_);
  if (!postBinary(url, payload.data(), payload.size(), response, true, status)) return false;
  if (status == 403) { resetSession(); return false; }
  if (status != 200 || response.size() < 48 || ((response.size() - 32) % 16) != 0) return false;

  std::vector<uint8_t> responseSignInput(28 + 4 + response.size() - 32);
  memcpy(responseSignInput.data(), signatureKey_, 28);
  memcpy(responseSignInput.data() + 28, seqBytes, 4);
  memcpy(responseSignInput.data() + 32, response.data() + 32, response.size() - 32);
  uint8_t expectedSignature[32];
  sha256(responseSignInput.data(), responseSignInput.size(), expectedSignature);
  if (!constantTimeEqual(expectedSignature, response.data(), sizeof(expectedSignature))) return false;

  std::vector<uint8_t> decrypted(response.size() - 32);
  mbedtls_aes_init(&aes); mbedtls_aes_setkey_dec(&aes, key_, 128);
  uint8_t decIv[16]; memcpy(decIv, iv, 16);
  const int aesResult = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, decrypted.size(), decIv,
                                               response.data() + 32, decrypted.data());
  mbedtls_aes_free(&aes);
  if (aesResult != 0 || decrypted.empty()) return false;
  const uint8_t pad = decrypted.back();
  if (!validPkcs7Padding(decrypted.data(), decrypted.size())) return false;
  plaintext = String(reinterpret_cast<const char*>(decrypted.data()), decrypted.size() - pad);
  return true;
}

bool TapoAdapter::request(const char* method, JsonObjectConst params, JsonDocument& response, bool retry) {
  if (connection_ != ConnectionState::Online || !sessionExpiresMs_ || deadlineReached(millis(), sessionExpiresMs_)) {
    if (!handshake()) return false;
  }
  JsonDocument requestDoc;
  requestDoc["method"] = method;
  requestDoc["request_time_milis"] = static_cast<uint64_t>(millis());
  requestDoc["terminal_uuid"] = "Q2FyZHB1dGVyLUFkdmFuY2Vk";
  if (!params.isNull() && params.size()) requestDoc["params"].set(params);
  String requestText, responseText;
  serializeJson(requestDoc, requestText);
  if (!encryptedPost(requestText, responseText)) {
    connection_ = ConnectionState::Offline;
    setError("KLAP encrypted request failed", millis());
    if (retry && handshake()) return request(method, params, response, false);
    return false;
  }
  if (deserializeJson(response, responseText) != DeserializationError::Ok) return false;
  const int errorCode = response["error_code"] | -1;
  if (errorCode != 0) {
    if (errorCode == -1501) connection_ = ConnectionState::AuthFailed;
    setError(errorCode == -1501 ? "KLAP authentication failed" : "Tapo returned an error", millis());
    return false;
  }
  connection_ = ConnectionState::Online;
  health_.lastError[0] = '\0';
  return true;
}

void TapoAdapter::updateState(JsonObjectConst result) {
  state_.power = result["device_on"] | state_.power;
  state_.poweredOnDevices = state_.power ? 1 : 0;
  state_.brightness = result["brightness"] | state_.brightness;
  state_.hue = result["hue"] | state_.hue;
  state_.saturation = result["saturation"] | state_.saturation;
  state_.colorTemperature = result["color_temp"] | state_.colorTemperature;
  state_.effectActive = result["dynamic_light_effect_enable"] | false;
  const char* effectId = result["dynamic_light_effect_id"] | "";
  state_.effect = !state_.effectActive ? 0 : strcmp(effectId, "L1") == 0 ? 1 : strcmp(effectId, "L2") == 0 ? 2 : 255;
  state_.lastReportMs = millis();
  health_.lastSuccessMs = state_.lastReportMs;
}

bool TapoAdapter::loadPresets() {
  JsonDocument paramsDoc, response;
  paramsDoc["start_index"] = 0;
  if (!request("get_preset_rules", paramsDoc.as<JsonObjectConst>(), response)) return false;
  JsonObjectConst result = response["result"];
  JsonArrayConst states = result["states"];
  if (states.isNull()) states = result["preset_state"];
  presetCount_ = 0;
  for (JsonObjectConst item : states) {
    if (presetCount_ >= presets_.size()) break;
    auto& preset = presets_[presetCount_++];
    preset.valid = true;
    preset.brightness = item["brightness"] | -1;
    preset.colorTemperature = item["color_temp"] | -1;
    preset.hue = item["hue"] | -1;
    preset.saturation = item["saturation"] | -1;
  }
  if (presetCount_ == 0) {
    for (int value : result["brightness"].as<JsonArrayConst>()) {
      if (presetCount_ >= presets_.size()) break;
      auto& preset = presets_[presetCount_++];
      preset.valid = true;
      preset.brightness = value;
    }
  }
  return presetCount_ > 0;
}

bool TapoAdapter::refreshGroup(uint32_t nowMs) {
  const auto hosts = discoverL530Hosts();
  if (hosts.empty()) {
    nextGroupDiscoveryMs_ = nowMs + groupDiscoveryRetryMs_;
    groupDiscoveryRetryMs_ = nextReconnectDelay(groupDiscoveryRetryMs_);
    setError("no L530E discovered", nowMs);
    updateGroupState();
    return !children_.empty();
  }
  groupDiscoveryRetryMs_ = 1000;
  nextGroupDiscoveryMs_ = nowMs + 60000;

  std::vector<std::string> current;
  for (const auto& child : children_) current.emplace_back(child->runtimeHost_.c_str());
  auto sortedHosts = hosts;
  std::sort(current.begin(), current.end());
  std::sort(sortedHosts.begin(), sortedHosts.end());
  if (current == sortedHosts) {
    updateGroupState();
    return true;
  }

  std::vector<std::unique_ptr<TapoAdapter>> discovered;
  for (const auto& host : sortedHosts) {
    auto child = std::make_unique<TapoAdapter>(false);
    child->runtimeHost_ = host.c_str();
    if (child->begin(baseConfig_)) discovered.push_back(std::move(child));
  }
  if (!discovered.empty()) children_ = std::move(discovered);
  updateGroupState();
  return !children_.empty();
}

void TapoAdapter::updateGroupState() {
  if (!groupMode_) return;
  LightState aggregate{};
  aggregate.totalDevices = static_cast<uint8_t>(min<size_t>(children_.size(), 255));
  bool haveFirst = false;
  bool authFailed = false;
  for (const auto& child : children_) {
    authFailed = authFailed || child->connectionState() == ConnectionState::AuthFailed;
    if (child->connectionState() != ConnectionState::Online) continue;
    const LightState& item = child->snapshot();
    ++aggregate.onlineDevices;
    if (!haveFirst) {
      const uint8_t total = aggregate.totalDevices;
      const uint8_t online = aggregate.onlineDevices;
      aggregate = item;
      aggregate.totalDevices = total;
      aggregate.onlineDevices = online;
      aggregate.poweredOnDevices = item.power ? 1 : 0;
      haveFirst = true;
    } else {
      if (item.power) ++aggregate.poweredOnDevices;
      if (aggregate.power != item.power) aggregate.mixedFields |= 0x01;
      if (aggregate.brightness != item.brightness) aggregate.mixedFields |= 0x02;
      if (aggregate.colorTemperature != item.colorTemperature || aggregate.hue != item.hue ||
          aggregate.saturation != item.saturation || aggregate.effect != item.effect)
        aggregate.mixedFields |= 0x04;
      aggregate.mixed = aggregate.mixedFields != 0;
    }
    aggregate.lastReportMs = max(aggregate.lastReportMs, item.lastReportMs);
  }
  state_ = aggregate;
  health_.lastSuccessMs = aggregate.lastReportMs;
  if (state_.onlineDevices == 0)
    connection_ = authFailed ? ConnectionState::AuthFailed : ConnectionState::Offline;
  else if (state_.onlineDevices < state_.totalDevices)
    connection_ = ConnectionState::Degraded;
  else
    connection_ = ConnectionState::Online;
  if (connection_ == ConnectionState::Online) health_.lastError[0] = '\0';
}

bool TapoAdapter::rememberPending(const DeviceCommand& command, uint32_t nowMs) {
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

bool TapoAdapter::commandMatches(const DeviceCommand& command) const {
  switch (command.kind) {
    case CommandKind::SetPower: return state_.power == (command.value1 != 0);
    case CommandKind::SetBrightness: return state_.power && state_.brightness == command.value1;
    case CommandKind::SetColorTemperature:
      return state_.power && state_.colorTemperature == command.value1;
    case CommandKind::SetHsv:
      return state_.power && state_.colorTemperature == 0 && state_.hue == command.value1 &&
             state_.saturation == command.value2 && state_.brightness == command.value3;
    case CommandKind::SetLightEffect: return state_.effect == command.value1;
    case CommandKind::ApplyLightPreset: return state_.presetIndex == command.value1;
    case CommandKind::Refresh: return state_.lastReportMs != 0;
    default: return false;
  }
}

void TapoAdapter::resolveChildPending(uint32_t nowMs) {
  for (auto& item : pending_) {
    if (!item.active) continue;
    if (state_.lastReportMs && deadlineReached(state_.lastReportMs, item.issuedMs) && commandMatches(item.command)) {
      events_.push(makeEvent(DeviceId::Light, item.command.id, EventResult::Succeeded, "KLAP readback confirmed",
                             ConfirmationKind::KlapReadback, 1, 1));
      item.active = false;
    } else if (deadlineReached(nowMs, item.deadlineMs)) {
      const EventResult result = connection_ == ConnectionState::AuthFailed ? EventResult::AuthFailed :
                                 connection_ == ConnectionState::Offline ? EventResult::Offline : EventResult::TimedOut;
      events_.push(makeEvent(DeviceId::Light, item.command.id, result, "KLAP readback timeout",
                             ConfirmationKind::KlapReadback, 0, 1));
      setError("KLAP command confirmation timeout", nowMs);
      item.active = false;
    }
  }
}

void TapoAdapter::collectChildEvents(uint32_t nowMs) {
  for (auto& child : children_) {
    DeviceEvent childEvent;
    while (child->pollEvent(childEvent)) {
      for (auto& group : groupPending_) {
        if (!group.active || group.commandId != childEvent.commandId) continue;
        ++group.completed;
        if (childEvent.result == EventResult::Succeeded) ++group.succeeded;
        if (childEvent.result == EventResult::AuthFailed) ++group.authFailures;
        break;
      }
    }
  }
  resolveGroupPending(nowMs);
}

void TapoAdapter::resolveGroupPending(uint32_t nowMs) {
  for (auto& group : groupPending_) {
    if (!group.active) continue;
    if (group.completed < group.targets && !deadlineReached(nowMs, group.deadlineMs)) continue;
    EventResult result = EventResult::TimedOut;
    const char* message = "all light confirmations timed out";
    if (group.succeeded == group.targets) {
      result = EventResult::Succeeded;
      message = "all lights confirmed";
    } else if (group.succeeded > 0) {
      result = EventResult::PartiallySucceeded;
      message = "some lights confirmed";
    } else if (group.authFailures > 0) {
      result = EventResult::AuthFailed;
      message = "light authentication failed";
    }
    events_.push(makeEvent(DeviceId::Light, group.commandId, result, message,
                           ConfirmationKind::KlapReadback, group.succeeded, group.targets));
    if (result != EventResult::Succeeded) setError(message, nowMs);
    group.active = false;
  }
}

void TapoAdapter::tick(uint32_t nowMs) {
  if (groupMode_) {
    for (auto& child : children_) child->tick(nowMs);
    collectChildEvents(nowMs);
    updateGroupState();
    if (!nextGroupDiscoveryMs_ || deadlineReached(nowMs, nextGroupDiscoveryMs_)) refreshGroup(nowMs);
    return;
  }
  if (!nextPollMs_ || deadlineReached(nowMs, nextPollMs_)) {
    JsonDocument params, response;
    if (request("get_device_info", params.as<JsonObjectConst>(), response))
      updateState(response["result"].as<JsonObjectConst>());
    nextPollMs_ = nowMs + 5000;
  }
  resolveChildPending(nowMs);
}

DeviceEvent TapoAdapter::execute(const DeviceCommand& command) {
  if (groupMode_) {
    DeviceCommand groupCommand = command;
    if (groupCommand.kind == CommandKind::TogglePower) {
      groupCommand.kind = CommandKind::SetPower;
      groupCommand.value1 = lightGroupPowerTarget(state_);
    }
    uint8_t accepted = 0;
    uint8_t targets = 0;
    uint8_t completed = 0;
    uint8_t succeeded = 0;
    uint8_t authFailures = 0;
    for (auto& child : children_) {
      if (child->connectionState() != ConnectionState::Online) continue;
      ++targets;
      const DeviceEvent result = child->execute(groupCommand);
      if (result.result == EventResult::Pending || result.result == EventResult::Succeeded) ++accepted;
      if (result.result == EventResult::Succeeded) ++succeeded;
      if (result.result == EventResult::AuthFailed) ++authFailures;
      if (result.result != EventResult::Pending) ++completed;
    }
    updateGroupState();
    char message[64];
    snprintf(message, sizeof(message), "%u/%u lights queued", accepted,
             static_cast<unsigned>(children_.size()));
    if (accepted) {
      GroupPending* slot = nullptr;
      for (auto& item : groupPending_) if (!item.active) { slot = &item; break; }
      if (!slot) return makeEvent(DeviceId::Light, command.id, EventResult::TimedOut, "group pending table full");
      slot->active = true;
      slot->commandId = command.id;
      slot->deadlineMs = millis() + 3000;
      slot->targets = targets;
      slot->completed = completed;
      slot->succeeded = succeeded;
      slot->authFailures = authFailures;
      return makeEvent(DeviceId::Light, command.id, EventResult::Pending, message,
                       ConfirmationKind::KlapReadback, 0, targets);
    }
    return makeEvent(DeviceId::Light, command.id,
                     authFailures ? EventResult::AuthFailed : EventResult::Offline,
                     children_.empty() ? "no L530E discovered" : "all lights offline",
                     ConfirmationKind::KlapReadback, 0, targets);
  }
  JsonDocument paramsDoc, response;
  JsonObject params = paramsDoc.to<JsonObject>();
  const char* method = "set_device_info";
  int appliedPreset = -1;
  switch (command.kind) {
    case CommandKind::TogglePower: params["device_on"] = !state_.power; break;
    case CommandKind::SetPower: params["device_on"] = command.value1 != 0; break;
    case CommandKind::SetBrightness: params["brightness"] = max<int32_t>(1, min<int32_t>(100, command.value1)); params["device_on"] = true; break;
    case CommandKind::SetColorTemperature:
      if (command.value1 < 2500 || command.value1 > 6500) return makeEvent(DeviceId::Light, command.id, EventResult::Unsupported, "2500..6500K");
      params["color_temp"] = command.value1; params["device_on"] = true; break;
    case CommandKind::SetHsv:
      if (command.value1 < 0 || command.value1 > 360 || command.value2 < 0 || command.value2 > 100)
        return makeEvent(DeviceId::Light, command.id, EventResult::Unsupported, "bad HSV");
      params["hue"] = command.value1; params["saturation"] = command.value2;
      params["brightness"] = max<int32_t>(1, min<int32_t>(100, command.value3)); params["color_temp"] = 0; params["device_on"] = true;
      break;
    case CommandKind::SetLightEffect:
      if (command.value1 < 0 || command.value1 > 2)
        return makeEvent(DeviceId::Light, command.id, EventResult::Unsupported, "effect 0..2");
      method = "set_dynamic_light_effect_rule_enable";
      params["enable"] = command.value1 != 0;
      if (command.value1 != 0) params["id"] = command.value1 == 1 ? "L1" : "L2";
      break;
    case CommandKind::ApplyLightPreset: {
      const int index = command.value1;
      if (index < 0 || index >= presetCount_ || !presets_[index].valid)
        return makeEvent(DeviceId::Light, command.id, EventResult::Unsupported, "preset unavailable");
      const auto& preset = presets_[index];
      params["device_on"] = true;
      if (preset.brightness >= 0) params["brightness"] = preset.brightness;
      if (preset.colorTemperature >= 0) params["color_temp"] = preset.colorTemperature;
      if (preset.hue >= 0) params["hue"] = preset.hue;
      if (preset.saturation >= 0) params["saturation"] = preset.saturation;
      appliedPreset = index;
      break;
    }
    case CommandKind::Refresh:
      nextPollMs_ = 0;
      if (!rememberPending(command, millis()))
        return makeEvent(DeviceId::Light, command.id, EventResult::TimedOut, "pending table full");
      return makeEvent(DeviceId::Light, command.id, EventResult::Pending, "refresh queued");
    default: return makeEvent(DeviceId::Light, command.id, EventResult::Unsupported, "unsupported command");
  }
  if (!request(method, params, response)) {
    const EventResult result = connection_ == ConnectionState::AuthFailed ? EventResult::AuthFailed : EventResult::Offline;
    return makeEvent(DeviceId::Light, command.id, result, "KLAP request failed");
  }
  if (appliedPreset >= 0) state_.presetIndex = static_cast<int8_t>(appliedPreset);
  nextPollMs_ = 0;
  if (!rememberPending(command, millis()))
    return makeEvent(DeviceId::Light, command.id, EventResult::TimedOut, "pending table full");
  return makeEvent(DeviceId::Light, command.id, EventResult::Pending, "awaiting readback");
}

void TapoAdapter::resetSession() {
  sessionCookie_ = "";
  sessionExpiresMs_ = 0;
  memset(key_, 0, sizeof(key_)); memset(ivPrefix_, 0, sizeof(ivPrefix_)); memset(signatureKey_, 0, sizeof(signatureKey_));
}

}  // namespace chc
