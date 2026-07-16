# Changelog

All notable project changes are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions use Semantic Versioning concepts; pre-1.0 release candidates may still change configuration or protocol behavior.

## [Unreleased]

### Added

- Professional bilingual project documentation and GitHub community infrastructure.
- MIT License for the original project source.

## [1.0.0-rc3] - 2026-07-16

### Added

- Dedicated priority-3 IR task and independent property-aware dispatcher.
- Queue-to-IR latency diagnostics with a 50 ms target and warning output.
- SaaS-inspired dark card UI with global dashboard and device detail pages.
- Battery and charging display on every normal-operation page.
- BMI270 motion wake, 30-second dimming, and 120-second display sleep.
- Complete Dyson air-quality page and redacted system diagnostics.
- Transactional dual-slot NVS configuration with schema 1/2 compatibility.
- Last-sent RG57A inferred-state persistence.
- Tapo group confirmation counts and mixed-property reporting.
- Python real-device probe and reversible write/readback gate.

### Changed

- Dyson and Tapo use automatic LAN discovery instead of configured device IPs.
- Absolute controls coalesce by property while one-shot IR actions preserve order.
- Dyson and Tapo commands require MQTT or KLAP readback before full success.
- Tapo KLAP handling validates response integrity, PKCS#7 padding, and response limits.

### Fixed

- Mixed-state Tapo groups can be switched fully off with one command.
- RG57A Swing, Turbo, Eco, Clean, and LED transitions always emit one-shot frames.
- RG57A temperature encoding remains Celsius and no longer produces Fahrenheit-like panel values.
- IR commands are no longer delayed by Tapo polling/discovery or Dyson reconnect work.
- Rapid temperature, speed, and brightness changes retain the newest target without filling the queue.

### Security

- Setup responses use `no-store` and validate field lengths and formats.
- Logs and generated reports exclude credentials and session material.
- KLAP response processing is length-bounded and integrity checked.

### Known limitations

- RG57A state is inferred because IR has no receive path.
- Flash/NVS encryption is not enabled.
- Same-LAN IPv4 reachability, UDP broadcast, and mDNS are required.

[Unreleased]: https://github.com/zx90316/Cardputer-Home-Controler/compare/v1.0.0-rc3...HEAD
[1.0.0-rc3]: https://github.com/zx90316/Cardputer-Home-Controler/releases/tag/v1.0.0-rc3
