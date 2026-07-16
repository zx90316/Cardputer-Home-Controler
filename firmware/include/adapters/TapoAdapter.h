#pragma once

#include <ArduinoJson.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/DeviceAdapter.h"
#include "core/EventBuffer.h"

namespace chc {

class TapoAdapter final : public DeviceAdapter {
 public:
  explicit TapoAdapter(bool groupMode = true) : groupMode_(groupMode) {}
  bool begin(const AppConfig& config) override;
  void tick(uint32_t nowMs) override;
  DeviceEvent execute(const DeviceCommand& command) override;
  bool pollEvent(DeviceEvent& event) override { return events_.pop(event); }
  ConnectionState connectionState() const override { return connection_; }
  AdapterHealth health() const override;
  const LightState& snapshot() const { return state_; }

 private:
  static std::vector<std::string> discoverL530Hosts(uint32_t timeoutMs = 3500);
  bool refreshGroup(uint32_t nowMs);
  void updateGroupState();
  bool handshake();
  bool request(const char* method, JsonObjectConst params, JsonDocument& response, bool retry = true);
  bool encryptedPost(const String& json, String& plaintext);
  bool postBinary(const String& url, const uint8_t* data, size_t length, std::vector<uint8_t>& response,
                  bool includeCookie, int& status, String* setCookie = nullptr);
  void deriveSession();
  void authHashV2();
  void updateState(JsonObjectConst result);
  bool loadPresets();
  void resetSession();
  bool rememberPending(const DeviceCommand& command, uint32_t nowMs);
  bool commandMatches(const DeviceCommand& command) const;
  void resolveChildPending(uint32_t nowMs);
  void collectChildEvents(uint32_t nowMs);
  void resolveGroupPending(uint32_t nowMs);
  void setError(const char* message, uint32_t nowMs);
  static void sha1(const uint8_t* data, size_t length, uint8_t out[20]);
  static void sha256(const uint8_t* data, size_t length, uint8_t out[32]);
  static void putBe32(uint8_t out[4], uint32_t value);

  TapoConfig config_;
  AppConfig baseConfig_{};
  LightState state_{};
  ConnectionState connection_{ConnectionState::Unconfigured};
  uint8_t localSeed_[16]{};
  uint8_t remoteSeed_[16]{};
  uint8_t authHash_[32]{};
  uint8_t key_[16]{};
  uint8_t ivPrefix_[12]{};
  uint8_t signatureKey_[28]{};
  int32_t sequence_{0};
  String sessionCookie_;
  uint32_t sessionExpiresMs_{0};
  uint32_t nextPollMs_{0};
  struct CachedPreset {
    bool valid{false};
    int16_t brightness{-1};
    int16_t colorTemperature{-1};
    int16_t hue{-1};
    int16_t saturation{-1};
  };
  std::array<CachedPreset, 7> presets_{};
  uint8_t presetCount_{0};
  bool groupMode_{true};
  String runtimeHost_;
  std::vector<std::unique_ptr<TapoAdapter>> children_;
  uint32_t nextGroupDiscoveryMs_{0};
  uint32_t groupDiscoveryRetryMs_{1000};
  AdapterHealth health_{};
  struct PendingCommand {
    bool active{false};
    DeviceCommand command{};
    uint32_t issuedMs{0};
    uint32_t deadlineMs{0};
  };
  struct GroupPending {
    bool active{false};
    uint32_t commandId{0};
    uint32_t deadlineMs{0};
    uint8_t targets{0};
    uint8_t completed{0};
    uint8_t succeeded{0};
    uint8_t authFailures{0};
  };
  std::array<PendingCommand, 8> pending_{};
  std::array<GroupPending, 8> groupPending_{};
  EventBuffer<DeviceEvent, 20> events_;
};

}  // namespace chc
