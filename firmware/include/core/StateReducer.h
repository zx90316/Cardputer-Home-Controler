#pragma once

#include "core/Models.h"

namespace chc {

inline void reduceAc(AcState& state, const DeviceCommand& command, uint32_t nowMs) {
  switch (command.kind) {
    case CommandKind::TogglePower: state.power = !state.power; break;
    case CommandKind::SetPower: state.power = command.value1 != 0; break;
    case CommandKind::SetTemperature:
      if (command.value1 >= 17 && command.value1 <= 30) state.temperature = static_cast<uint8_t>(command.value1);
      break;
    case CommandKind::SetAcMode:
      if (command.value1 >= 0 && command.value1 <= static_cast<int>(AcMode::Fan))
        state.mode = static_cast<AcMode>(command.value1);
      break;
    case CommandKind::SetFanSpeed:
      if (command.value1 >= 0 && command.value1 <= static_cast<int>(FanSpeed::High))
        state.fan = static_cast<FanSpeed>(command.value1);
      break;
    case CommandKind::ToggleSwing: state.swingVertical = !state.swingVertical; break;
    case CommandKind::ToggleSleep: state.sleep = !state.sleep; break;
    case CommandKind::ToggleTurbo: state.turbo = !state.turbo; break;
    case CommandKind::ToggleEco: state.eco = !state.eco; break;
    case CommandKind::ToggleClean: state.clean = !state.clean; break;
    case CommandKind::ToggleLed: state.led = !state.led; break;
    case CommandKind::SetAcOnTimer:
      if (command.value1 >= 0 && command.value1 <= 1440)
        state.onTimerMinutes = static_cast<uint16_t>(command.value1);
      break;
    case CommandKind::SetAcOffTimer:
      if (command.value1 >= 0 && command.value1 <= 1440)
        state.offTimerMinutes = static_cast<uint16_t>(command.value1);
      break;
    default: break;
  }
  state.lastSentMs = nowMs;
}

inline uint8_t clampPercent(int32_t value) {
  if (value < 1) return 1;
  if (value > 100) return 100;
  return static_cast<uint8_t>(value);
}

}  // namespace chc
