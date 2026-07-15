#pragma once

#include <ir_Midea.h>

#include "core/DeviceAdapter.h"
#include "core/EventBuffer.h"
#include "core/StateReducer.h"

namespace chc {

class IrAdapter final : public DeviceAdapter {
 public:
  static constexpr uint8_t kIrPin = 44;

  IrAdapter() : ac_(kIrPin) {}
  bool begin(const AppConfig& config) override;
  void tick(uint32_t nowMs) override;
  DeviceEvent execute(const DeviceCommand& command) override;
  bool pollEvent(DeviceEvent& event) override { return events_.pop(event); }
  ConnectionState connectionState() const override { return ConnectionState::Online; }
  AdapterHealth health() const override { return health_; }
  const AcState& snapshot() const { return state_; }
  void restoreState(const AcState& state) { state_ = state; }

 private:
  void applyState();
  IRMideaAC ac_;
  AcState state_{};
  AdapterHealth health_{ConnectionState::Online, 0, 0, 0, 1, 1, {}};
  EventBuffer<DeviceEvent, 4> events_;
};

}  // namespace chc
