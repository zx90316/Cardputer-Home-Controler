#pragma once

#include <cstdint>
#include <cstring>

namespace chc {

enum class DeviceId : uint8_t { Ac = 1, Dyson = 2, Light = 3 };
enum class ConnectionState : uint8_t { Unconfigured, Connecting, Online, Degraded, Offline, AuthFailed };
enum class EventResult : uint8_t { Pending, Succeeded, PartiallySucceeded, TimedOut, AuthFailed, Offline, Unsupported };
enum class ConfirmationKind : uint8_t { None, AssumedIr, MqttState, KlapReadback };

enum class AcMode : uint8_t { Auto, Cool, Dry, Fan, Heat };
enum class FanSpeed : uint8_t { Auto, Low, Medium, High };

enum class CommandKind : uint8_t {
  TogglePower,
  SetPower,
  SetTemperature,
  SetAcMode,
  SetFanSpeed,
  ToggleSwing,
  ToggleSleep,
  ToggleTurbo,
  ToggleEco,
  ToggleClean,
  ToggleLed,
  SetAcOnTimer,
  SetAcOffTimer,
  SetBrightness,
  SetColorTemperature,
  SetHsv,
  SetLightEffect,
  ApplyLightPreset,
  SetDysonAuto,
  SetOscillation,
  SetOscillationAngles,
  SetAirflowFront,
  SetNightMode,
  SetContinuousMonitoring,
  SetSleepTimer,
  Refresh,
};

struct DeviceCommand {
  DeviceId device{DeviceId::Ac};
  CommandKind kind{CommandKind::Refresh};
  int32_t value1{0};
  int32_t value2{0};
  int32_t value3{0};
  uint32_t id{0};
};

struct AcState {
  bool power{false};
  uint8_t temperature{24};
  AcMode mode{AcMode::Cool};
  FanSpeed fan{FanSpeed::Auto};
  bool swingVertical{false};
  bool sleep{false};
  bool turbo{false};
  bool eco{false};
  bool clean{false};
  bool led{true};
  uint16_t onTimerMinutes{0};
  uint16_t offTimerMinutes{0};
  uint32_t lastSentMs{0};
};

struct DysonState {
  bool power{false};
  bool autoMode{false};
  uint8_t speed{1};
  bool oscillation{false};
  uint16_t angleLow{45};
  uint16_t angleHigh{315};
  bool frontAirflow{true};
  bool nightMode{false};
  bool continuousMonitoring{false};
  int16_t sleepTimerMinutes{-1};
  float temperatureC{0};
  uint8_t humidity{0};
  uint16_t pm25{0};
  uint16_t pm10{0};
  uint16_t voc{0};
  uint16_t no2{0};
  uint16_t formaldehyde{0};
  int16_t carbonFilter{-1};
  int16_t hepaFilter{-1};
  uint32_t lastReportMs{0};
};

struct LightState {
  bool power{false};
  uint8_t brightness{100};
  uint16_t colorTemperature{4000};
  uint16_t hue{0};
  uint8_t saturation{0};
  bool effectActive{false};
  uint8_t effect{0};  // 0=off, 1=Party/L1, 2=Relax/L2.
  int8_t presetIndex{-1};
  uint8_t onlineDevices{0};
  uint8_t totalDevices{0};
  bool mixed{false};
  uint32_t lastReportMs{0};
};

struct DeviceEvent {
  DeviceId device{DeviceId::Ac};
  uint32_t commandId{0};
  EventResult result{EventResult::Pending};
  ConfirmationKind confirmation{ConfirmationKind::None};
  uint8_t succeededTargets{0};
  uint8_t totalTargets{0};
  char message[64]{};
};

struct AdapterHealth {
  ConnectionState connection{ConnectionState::Unconfigured};
  uint32_t lastSuccessMs{0};
  uint32_t lastErrorMs{0};
  uint32_t nextRetryMs{0};
  uint8_t onlineTargets{0};
  uint8_t totalTargets{0};
  char lastError[64]{};
};

inline DeviceEvent makeEvent(DeviceId device, uint32_t id, EventResult result, const char* text,
                             ConfirmationKind confirmation = ConfirmationKind::None,
                             uint8_t succeededTargets = 0, uint8_t totalTargets = 0) {
  DeviceEvent event;
  event.device = device;
  event.commandId = id;
  event.result = result;
  event.confirmation = confirmation;
  event.succeededTargets = succeededTargets;
  event.totalTargets = totalTargets;
  if (text) std::strncpy(event.message, text, sizeof(event.message) - 1);
  return event;
}

}  // namespace chc
