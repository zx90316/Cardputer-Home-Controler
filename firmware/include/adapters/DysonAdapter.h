#pragma once

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <array>

#include "core/DeviceAdapter.h"
#include "core/EventBuffer.h"

namespace chc {

class DysonAdapter final : public DeviceAdapter {
 public:
  DysonAdapter();
  bool begin(const AppConfig& config) override;
  void tick(uint32_t nowMs) override;
  DeviceEvent execute(const DeviceCommand& command) override;
  bool pollEvent(DeviceEvent& event) override { return events_.pop(event); }
  ConnectionState connectionState() const override { return connection_; }
  AdapterHealth health() const override;
  const DysonState& snapshot() const { return state_; }

 private:
  static DysonAdapter* instance_;
  static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);
  void onMessage(const uint8_t* payload, size_t length);
  bool reconnect(uint32_t nowMs);
  bool discoverHost();
  bool publishStateSet(JsonObjectConst data);
  bool publishRequest(const char* message);
  String commandTopic() const;
  String statusTopic() const;
  static String mqttTimestamp();
  static int fieldInt(JsonVariantConst value, int fallback = 0);
  static const char* fieldText(JsonVariantConst value, const char* fallback = "");
  bool rememberPending(const DeviceCommand& command, uint32_t nowMs);
  bool commandMatches(const DeviceCommand& command) const;
  void resolvePending(uint32_t nowMs);
  void setError(const char* message, uint32_t nowMs);

  struct PendingCommand {
    bool active{false};
    DeviceCommand command{};
    uint32_t issuedMs{0};
    uint32_t deadlineMs{0};
  };

  WiFiClient network_;
  PubSubClient mqtt_;
  DysonConfig config_;
  DysonState state_{};
  ConnectionState connection_{ConnectionState::Unconfigured};
  uint32_t nextReconnectMs_{0};
  uint32_t reconnectDelayMs_{1000};
  uint32_t nextPollMs_{0};
  uint32_t nextDiscoveryMs_{0};
  String runtimeHost_;
  AdapterHealth health_{};
  std::array<PendingCommand, 8> pending_{};
  EventBuffer<DeviceEvent, 12> events_;
};

}  // namespace chc
