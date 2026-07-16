# Cardputer Adv hardware acceptance

The Python LAN gate has passed for the actual L530E and TP09. The following tests require the Cardputer Adv and are intentionally not claimed by the desktop build.

## Flash and setup

1. Flash `firmware/build/cardputer-home-controller-complete.bin` at offset `0x0` over USB.
2. Confirm display, keyboard and random-password WPA2 setup AP.
3. Submit known-good LAN settings without entering any device IP. Confirm invalid Tapo or Dyson credentials are rejected without replacing NVS.
4. Reboot and confirm settings persist. Hold the physical `Esc/\`` key for five seconds during boot and confirm setup mode returns.

## RG57A / GPIO 44 infrared

Aim the Cardputer IR emitter at the indoor unit and test power, Auto/Cool/Dry/Fan, 17/23/30 °C, Auto/Low/Medium/High fan, vertical swing, Sleep, Turbo, Eco, Clean, LED and 30/60/120 minute on/off timers.

While Tapo discovery is running, with one lamp offline, and while TP09 reconnects, press A/C controls at least 20 times. Serial must report `ir dispatch ... latency_ms=... target_ms=50` without `WARNING`; the diagnostics page maximum IR latency must remain at or below 50 ms. IR must emit immediately rather than after the network operation completes.

For Swing, Turbo, Eco, Clean and LED, press each control twice and confirm both the on and off transition. Reboot after setting a recognizable temperature/mode and confirm the UI restores it as an explicitly assumed last-sent state. Press temperature up/down ten times rapidly and confirm the final value matches all ten presses.

Stop A/C UI expansion if the first power or mode tests fail. Record the full remote suffix and capture the real signal with an IR receiver before changing protocol code. Heat remains disabled.

## L530E and TP09

Confirm the Tapo page reports the expected online/total count, then verify one command changes every L530E. For every exposed command, confirm a device readback or MQTT state. Check L530E Off/Party/Relax and all seven stored presets. Check TP09 sensor data for temperature, humidity, PM2.5, PM10, VOC, NO₂, formaldehyde and filter life.

Use `4` to verify the complete Dyson air-quality page and `I` to verify diagnostics contain no secret values. Rapidly press Dyson speed and group brightness ten times; the final value must match all presses without a queue-full warning. With one lamp unavailable, a group command must report the successful/target count rather than reporting full success.

Create a mixed lighting state with at least one online L530E on and one off. Press group power once and confirm every online lamp turns off. From that all-off state, press twice rapidly and confirm the final target is all-off again, without a queue-full warning. Repeat from all-on and confirm the final target remains all-on.

Disconnect WAN while keeping the LAN up; all three device families must remain controllable. Restart the router or change DHCP addresses and verify UDP/mDNS rediscovery without editing settings. Tapo discovery runs every 60 seconds; Dyson triggers mDNS rediscovery after connection failures.

## Endurance and security

Confirm the battery percentage and charging marker appear on every page and reasonably match the device power state. Leave the Cardputer motionless: the display must dim after 30 seconds and sleep after 120 seconds while TP09 MQTT remains connected. Move the Cardputer and confirm the display wakes. Repeat using a key and confirm the wake key does not operate any appliance. The diagnostics page must report `IMU ON`; if it reports `IMU OFF`, motion wake must not be accepted as passed.

Run for 24 hours while operating all pages. Confirm the UI remains responsive, free/minimum heap does not continuously decline, there is only one TP09 MQTT connection, and serial logs contain no Wi-Fi, Tapo or Dyson secrets. Only after this passes may `hardware_validated` be changed to `true` and the RC be promoted to stable 1.0.
