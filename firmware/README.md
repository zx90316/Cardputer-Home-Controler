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

For an update from an earlier working build, use the application-only image at `0x10000` to preserve NVS settings. The complete merged image fills the partition gaps and therefore requires first setup again.

## First setup

An unconfigured unit enters setup automatically. On an already configured unit, hold the physical `Esc/\`` key throughout the five-second boot check.

1. Read the random AP password from the Cardputer screen.
2. Join `Cardputer-Home-Setup` from a phone or computer.
3. Open `http://192.168.4.1`.
4. Enter Wi-Fi, Tapo credentials, and TP09 serial/product type/local MQTT credential. No device IP is requested. The verified TP09 product type is `438K`.
5. The firmware joins the LAN while keeping the setup AP active, discovers every L530E using UDP and discovers TP09 using `_dyson_mqtt._tcp` mDNS. It then validates KLAP reads and a Dyson MQTT state report. NVS is written only if these checks pass.

The setup AP closes after ten minutes. Secrets are stored in ordinary NVS; flash encryption is not enabled.

## Global quick-control page

The firmware starts on a quick page that prints every available key on screen. Press `0` from any page to return to it.

- A/C: `Q` power, `W`/`E` temperature down/up, `R` next mode.
- Dyson: `A` power, `S`/`D` speed down/up, `F` oscillation, `G`/`H` shift the oscillation angle left/right.
- All Tapo lamps: `Z` power, `X` toggle warm white (2700 K)/cool white (6500 K), `C`/`V` brightness down/up.
- `Tab` cycles quick page, A/C details, Dyson details, and all-lights details. `1`, `2`, `3` open those detail pages directly.

Every Tapo shortcut is a group command for all discovered L530E lamps, not only the first lamp.

## Detail menu controls

The screen presents a scrollable Traditional Chinese menu and always shows the navigation keys in the footer:

- `Tab`: cycle the quick page, A/C, Dyson and all-lights pages. `0` returns to quick control; `1`, `2`, `3` open a device page directly.
- `W`/`S`: move the highlighted row.
- `A`/`D`: decrease/increase or change the selected value.
- `Enter`: toggle or apply the highlighted row.
- `Space`: power for the current page.

The Tapo page controls every discovered L530E with one operation and displays online/total counts. If lamps report different state it displays `狀態不一致`; the next group command synchronizes the selected property. Discovery repeats every 60 seconds so added lamps and DHCP address changes are picked up automatically.

The A/C page is explicitly an assumed/last-sent state because infrared has no feedback. Heat and Follow Me are disabled until the exact RG57A variant is confirmed on hardware.
