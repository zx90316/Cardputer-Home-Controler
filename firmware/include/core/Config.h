#pragma once

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <string>

namespace chc {

constexpr uint32_t kConfigSchemaVersion = 2;

struct WifiConfig {
  std::string ssid;
  std::string password;
};

struct TapoConfig {
  std::string username;
  std::string password;
};

struct DysonConfig {
  std::string serial;
  std::string productType{"438K"};
  std::string credential;
};

struct AppConfig {
  uint32_t schemaVersion{kConfigSchemaVersion};
  WifiConfig wifi;
  TapoConfig tapo;
  DysonConfig dyson;

  bool structurallyValid() const {
    const bool productTypeValid = dyson.productType.size() >= 2 && dyson.productType.size() <= 8 &&
        std::all_of(dyson.productType.begin(), dyson.productType.end(),
                    [](unsigned char value) { return std::isalnum(value) != 0; });
    return schemaVersion == kConfigSchemaVersion && !wifi.ssid.empty() && wifi.ssid.size() <= 32 &&
           wifi.password.size() <= 64 && !tapo.username.empty() && tapo.username.size() <= 254 &&
           !tapo.password.empty() && tapo.password.size() <= 128 &&
           !dyson.serial.empty() && dyson.serial.size() <= 32 && productTypeValid &&
           !dyson.credential.empty() && dyson.credential.size() <= 512;
  }
};

}  // namespace chc
