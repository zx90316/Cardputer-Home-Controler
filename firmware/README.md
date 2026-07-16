# Cardputer Adv firmware

This is a standalone Arduino/ESP32-S3 firmware. Home Assistant, a LAN server, and cloud access are not used during normal operation.

## Build

From the repository root in PowerShell:

```powershell
.\.venv\Scripts\python.exe -m pip install -r .\firmware\requirements.txt
.\scripts\build-firmware.ps1 -RunNativeTests
```

The script creates a temporary drive mapping when the repository path contains non-ASCII characters. This is required by the pinned ESP32 GCC toolchain on Windows and is removed after the build.

Artifacts:

- `build/cardputer-home-controller-complete.bin`: complete image, flash at offset `0x0`.
- `build/cardputer-home-controller-app.bin`: application-only image, flash at offset `0x10000`.
- `build/cardputer-home-controller-app-previous.bin`: rollback application retained before the first RC build.
- `build/firmware-manifest.json`: version, offsets, sizes and SHA-256 hashes.

For an update from an earlier working build, use the application-only image at `0x10000` to preserve NVS settings. The complete merged image fills the partition gaps and therefore requires first setup again.

## First setup

An unconfigured unit enters setup automatically. On an already configured unit, hold the physical `Esc/\`` key throughout the five-second boot check.

1. Read the random AP password from the Cardputer screen.
2. Join `Cardputer-Home-Setup` from a phone or computer.
3. Open `http://192.168.4.1`.
4. Enter Wi-Fi, Tapo credentials, and TP09 serial/product type/local MQTT credential. No device IP is requested. The verified TP09 product type is `438K`.
5. The firmware joins the LAN while keeping the setup AP active, discovers every L530E using UDP and discovers TP09 using `_dyson_mqtt._tcp` mDNS. It then validates KLAP reads and a Dyson MQTT state report. NVS is written only if these checks pass.

The setup AP closes after ten minutes. Secrets are stored in ordinary NVS; flash encryption is not enabled.
Configuration uses two checksummed NVS slots and automatically falls back to the last valid slot. Existing schema 1/2 settings remain readable. When setup times out, the unit restarts instead of remaining in an inert setup screen.

## SaaS-style control interface

The firmware starts on a dark card-based dashboard. Three live cards summarize A/C, Dyson and the complete Tapo group, including command status and the relevant shortcut keys. Every page has a compact connection indicator and a battery/charging indicator in its header. Press `0` from any page to return to the dashboard.

- A/C: `Q` power, `W`/`E` temperature down/up, `R` next mode.
- Dyson: `A` power, `S`/`D` speed down/up, `F` oscillation, `G`/`H` shift the oscillation angle left/right.
- All Tapo lamps: `Z` power, `X` toggle warm white (2700 K)/cool white (6500 K), `C`/`V` brightness down/up.
- `Tab` cycles quick, A/C, Dyson control, Dyson air-quality, all-lights and diagnostics pages. `1`, `2`, `3` open the device detail pages directly.
- `4` opens Dyson air-quality and filter data. `I` opens redacted LAN/system diagnostics.

Every Tapo shortcut is a group command for all discovered L530E lamps, not only the first lamp.
If the group is mixed, one power press switches every currently online lamp off. A second rapid press uses the pending target rather than a stale device snapshot, so it reliably switches the group back on.
The quick page uses `…`, `✓`, `△`, and `!` for pending, confirmed, partially confirmed, and failed/offline commands. Dyson requires an MQTT state report and Tapo requires KLAP readback within three seconds; IR remains explicitly assumed.

## Battery and power saving

- The header reads the Cardputer Adv battery level and shows a `+` while charging.
- After 30 seconds without keyboard or motion activity, display brightness drops to the power-saving level.
- After 120 seconds, the LCD enters sleep while Wi-Fi, MQTT, discovery and device control tasks remain active.
- Moving the Cardputer wakes the display through its onboard BMI270 IMU. A key press also wakes it; that first wake key is consumed and never sent as an appliance command.
- The diagnostics page reports battery, charging, IMU availability and active/power-save state.

This is display power saving rather than ESP32 deep sleep, intentionally preserving LAN connections and the three-second command-confirmation behavior.

## Detail menu controls

The screen presents a scrollable Traditional Chinese menu and always shows the navigation keys in the footer:

- `Tab`: cycle the quick page, A/C, Dyson and all-lights pages. `0` returns to quick control; `1`, `2`, `3` open a device page directly.
- `W`/`S`: move the highlighted row.
- `A`/`D`: decrease/increase or change the selected value.
- `Enter`: toggle or apply the highlighted row.
- `Space`: power for the current page.

The Tapo page controls every discovered L530E with one operation and displays online/total counts. If lamps report different state it displays `狀態不一致`; the next group command synchronizes the selected property. Discovery repeats every 60 seconds so added lamps and DHCP address changes are picked up automatically.

The A/C page is explicitly an assumed/last-sent state because infrared has no feedback. Heat and Follow Me are disabled until the exact RG57A variant is confirmed on hardware.
The last-sent A/C state is restored after reboot with a five-second write debounce. Swing, Turbo, Eco, Clean and LED are one-shot Midea toggle frames and are transmitted on both transitions.

## Infrared latency isolation

Starting with `1.0.0-rc3`, A/C commands use a dedicated property-aware dispatcher and a priority-3 FreeRTOS task. Tapo HTTP/KLAP polling, 3.5-second discovery windows, Dyson mDNS/MQTT reconnects and deferred NVS writes cannot block infrared transmission. Serial diagnostics report `ir dispatch ... latency_ms=... target_ms=50`; the diagnostics page shows the last/maximum latency as `IR last/max ms`.

`1.0.0-rc3` remains a release candidate until the hardware acceptance checklist, including motion wake, IR latency and the 24-hour endurance run, passes.
