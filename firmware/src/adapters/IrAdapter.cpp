#include "adapters/IrAdapter.h"

#include <Arduino.h>

#include "core/ProtocolHelpers.h"

namespace chc {

bool IrAdapter::begin(const AppConfig&) {
  ac_.begin();
  applyState();
  return true;
}

void IrAdapter::tick(uint32_t) {}

void IrAdapter::applyState() {
  ac_.setPower(state_.power);
  // IRMideaAC defaults to Fahrenheit unless explicitly told otherwise.
  // RG57A temperature values in our UI/state are always Celsius.
  ac_.setUseCelsius(true);
  ac_.setTemp(state_.temperature, true);
  switch (state_.mode) {
    case AcMode::Auto: ac_.setMode(kMideaACAuto); break;
    case AcMode::Cool: ac_.setMode(kMideaACCool); break;
    case AcMode::Dry: ac_.setMode(kMideaACDry); break;
    case AcMode::Fan: ac_.setMode(kMideaACFan); break;
    case AcMode::Heat: ac_.setMode(kMideaACCool); break;  // Heat intentionally disabled until RG57A verification.
  }
  switch (state_.fan) {
    case FanSpeed::Auto: ac_.setFan(kMideaACFanAuto); break;
    case FanSpeed::Low: ac_.setFan(kMideaACFanLow); break;
    case FanSpeed::Medium: ac_.setFan(kMideaACFanMed); break;
    case FanSpeed::High: ac_.setFan(kMideaACFanHigh); break;
  }
  ac_.setSleep(state_.sleep);
}

DeviceEvent IrAdapter::execute(const DeviceCommand& command) {
  if (command.device != DeviceId::Ac) return makeEvent(DeviceId::Ac, command.id, EventResult::Unsupported, "wrong device");
  if (command.kind == CommandKind::SetAcMode && command.value1 == static_cast<int>(AcMode::Heat))
    return makeEvent(DeviceId::Ac, command.id, EventResult::Unsupported, "heat disabled");

  reduceAc(state_, command, millis());
  applyState();
  const bool oneShotToggle = isMideaOneShotToggle(command.kind);
  switch (command.kind) {
    // These are one-shot Midea toggle frames, not absolute state fields. A true
    // pulse is required on every transition, including the transition to off.
    case CommandKind::ToggleSwing: ac_.setSwingVToggle(true); break;
    case CommandKind::ToggleEco: ac_.setEconoToggle(true); break;
    case CommandKind::ToggleClean: ac_.setCleanToggle(true); break;
    case CommandKind::ToggleLed: ac_.setLightToggle(true); break;
    case CommandKind::ToggleTurbo: ac_.setTurboToggle(true); break;
    case CommandKind::SetAcOnTimer: ac_.setOnTimer(state_.onTimerMinutes); break;
    case CommandKind::SetAcOffTimer: ac_.setOffTimer(state_.offTimerMinutes); break;
    default: break;
  }
  (void)oneShotToggle;  // Kept explicit for native policy coverage and future special frames.
  ac_.send();
  health_.lastSuccessMs = state_.lastSentMs;
  return makeEvent(DeviceId::Ac, command.id, EventResult::Succeeded, "assumed / last sent",
                   ConfirmationKind::AssumedIr, 1, 1);
}

}  // namespace chc
