# Third-party sources and licenses

- [python-kasa](https://github.com/python-kasa/python-kasa), GNU GPL v3: Python-only Tapo validation dependency. Firmware will implement the observed KLAP protocol independently and will not copy this source.
- [libdyson-rest](https://github.com/libdyson-wg/libdyson-rest), MIT: MyDyson OTP and local credential bootstrap.
- [paho-mqtt](https://github.com/eclipse-paho/paho.mqtt.python), EPL-2.0 / BSD-3-Clause: Python-only direct MQTT validation.
- [zeroconf](https://github.com/python-zeroconf/python-zeroconf), LGPL-2.1-or-later: Python-only TP09 mDNS discovery.
- [libdyson](https://github.com/libdyson-wg/libdyson), Apache-2.0: protocol behavior used as a reference; it is not a runtime dependency or copied source.
- [M5Cardputer](https://github.com/m5stack/M5Cardputer), MIT: Cardputer keyboard and board integration.
- [M5Unified](https://github.com/m5stack/M5Unified), MIT: display and M5 hardware abstraction.
- [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266), LGPL-2.1: Midea 48-bit IR generation.
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson), MIT: firmware JSON parsing and serialization.
- [PubSubClient](https://github.com/knolleary/pubsubclient), MIT: firmware MQTT 3.1 client.
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32), LGPL-2.1: ESP32-S3 Arduino framework, Wi-Fi, HTTP, NVS and bundled mbedTLS APIs.

Exact transitive Python and PlatformIO package licenses should be audited from the pinned environments before redistribution of a binary.
