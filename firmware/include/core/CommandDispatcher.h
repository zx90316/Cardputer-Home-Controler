#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/Models.h"

namespace chc {

inline bool commandIsCoalescable(CommandKind kind) {
  switch (kind) {
    case CommandKind::SetPower:
    case CommandKind::SetTemperature:
    case CommandKind::SetAcMode:
    case CommandKind::SetFanSpeed:
    case CommandKind::SetAcOnTimer:
    case CommandKind::SetAcOffTimer:
    case CommandKind::SetBrightness:
    case CommandKind::SetColorTemperature:
    case CommandKind::SetHsv:
    case CommandKind::SetLightEffect:
    case CommandKind::ApplyLightPreset:
    case CommandKind::SetDysonAuto:
    case CommandKind::SetOscillation:
    case CommandKind::SetOscillationAngles:
    case CommandKind::SetAirflowFront:
    case CommandKind::SetNightMode:
    case CommandKind::SetContinuousMonitoring:
    case CommandKind::SetSleepTimer:
    case CommandKind::Refresh:
      return true;
    default:
      return false;
  }
}

template <size_t Capacity>
class CommandDispatcher {
 public:
  bool push(const DeviceCommand& command) {
    if (commandIsCoalescable(command.kind)) {
      for (auto& entry : entries_) {
        if (entry.state == State::Queued && sameProperty(entry.command, command)) {
          entry.command = command;
          entry.sequence = nextSequence_++;
          return true;
        }
      }
    }
    for (auto& entry : entries_) {
      if (entry.state == State::Free) {
        entry.command = command;
        entry.sequence = nextSequence_++;
        entry.state = State::Queued;
        return true;
      }
    }
    return false;
  }

  bool take(DeviceCommand& command) {
    Entry* selected = nullptr;
    for (auto& candidate : entries_) {
      if (candidate.state != State::Queued || blockedByInflight(candidate.command)) continue;
      if (!selected || candidate.sequence < selected->sequence) selected = &candidate;
    }
    if (!selected) return false;
    selected->state = State::Inflight;
    command = selected->command;
    return true;
  }

  bool complete(uint32_t commandId) {
    for (auto& entry : entries_) {
      if (entry.state == State::Inflight && entry.command.id == commandId) {
        entry.state = State::Free;
        return true;
      }
    }
    return false;
  }

  bool latest(DeviceId device, CommandKind kind, DeviceCommand& command) const {
    const Entry* selected = nullptr;
    for (const auto& entry : entries_) {
      if (entry.state == State::Free || entry.command.device != device || entry.command.kind != kind) continue;
      if (!selected || entry.sequence > selected->sequence) selected = &entry;
    }
    if (!selected) return false;
    command = selected->command;
    return true;
  }

  size_t pendingCount(DeviceId device, CommandKind kind) const {
    size_t count = 0;
    for (const auto& entry : entries_)
      if (entry.state != State::Free && entry.command.device == device && entry.command.kind == kind) ++count;
    return count;
  }

  size_t size() const {
    size_t count = 0;
    for (const auto& entry : entries_) if (entry.state != State::Free) ++count;
    return count;
  }

 private:
  enum class State : uint8_t { Free, Queued, Inflight };
  struct Entry {
    DeviceCommand command{};
    uint32_t sequence{0};
    State state{State::Free};
  };

  static bool sameProperty(const DeviceCommand& left, const DeviceCommand& right) {
    return left.device == right.device && left.kind == right.kind;
  }

  bool blockedByInflight(const DeviceCommand& command) const {
    if (!commandIsCoalescable(command.kind)) return false;
    for (const auto& entry : entries_)
      if (entry.state == State::Inflight && sameProperty(entry.command, command)) return true;
    return false;
  }

  std::array<Entry, Capacity> entries_{};
  uint32_t nextSequence_{1};
};

}  // namespace chc
