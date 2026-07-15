# Cardputer Adv hardware acceptance

The Python LAN gate has passed for the actual L530E and TP09. The following tests require the Cardputer Adv and are intentionally not claimed by the desktop build.

## Flash and setup

1. Flash `firmware/build/cardputer-home-controller-complete.bin` at offset `0x0` over USB.
2. Confirm display, keyboard and random-password WPA2 setup AP.
3. Submit known-good LAN settings without entering any device IP. Confirm invalid Tapo or Dyson credentials are rejected without replacing NVS.
4. Reboot and confirm settings persist. Hold the physical `Esc/\`` key for five seconds during boot and confirm setup mode returns.

## RG57A / GPIO 44 infrared

Aim the Cardputer IR emitter at the indoor unit and test power, Auto/Cool/Dry/Fan, 17/23/30 °C, Auto/Low/Medium/High fan, vertical swing, Sleep, Turbo, Eco, Clean, LED and 30/60/120 minute on/off timers.

Stop A/C UI expansion if the first power or mode tests fail. Record the full remote suffix and capture the real signal with an IR receiver before changing protocol code. Heat remains disabled.

## L530E and TP09

Confirm the Tapo page reports the expected online/total count, then verify one command changes every L530E. For every exposed command, confirm a device readback or MQTT state. Check L530E Off/Party/Relax and all seven stored presets. Check TP09 sensor data for temperature, humidity, PM2.5, PM10, VOC, NO₂, formaldehyde and filter life.

Disconnect WAN while keeping the LAN up; all three device families must remain controllable. Restart the router or change DHCP addresses and verify UDP/mDNS rediscovery without editing settings. Tapo discovery runs every 60 seconds; Dyson triggers mDNS rediscovery after connection failures.

## Endurance and security

Run for two hours while operating all pages. Confirm the UI remains responsive, there is only one TP09 MQTT connection, and serial logs contain no Wi-Fi, Tapo or Dyson secrets.
