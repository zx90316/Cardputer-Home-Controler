#pragma once

#include "core/Config.h"
#include "core/Models.h"

namespace chc {

class DeviceAdapter {
 public:
  virtual ~DeviceAdapter() = default;
  virtual bool begin(const AppConfig& config) = 0;
  virtual void tick(uint32_t nowMs) = 0;
  virtual DeviceEvent execute(const DeviceCommand& command) = 0;
  virtual bool pollEvent(DeviceEvent& event) = 0;
  virtual ConnectionState connectionState() const = 0;
  virtual AdapterHealth health() const = 0;
};

}  // namespace chc
