# Support

English · [繁體中文](SUPPORT.zh-TW.md)

## Where to ask

- **Confirmed bug:** use the [bug report form](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=bug_report.yml).
- **Feature proposal:** use the [feature request form](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=feature_request.yml).
- **New hardware evidence:** use the hardware compatibility form.
- **Security issue:** do not open a public issue; follow [SECURITY.md](SECURITY.md).

This is a community project. Support is best effort and no response-time or device-compatibility guarantee is provided.

## Check before opening an issue

1. Confirm the controller and devices are on the same reachable IPv4 LAN.
2. Confirm UDP broadcast and mDNS are not blocked by guest-network or VLAN isolation.
3. Upgrade to the latest release candidate.
4. Reproduce after restarting the Cardputer and affected appliance.
5. Run the read-only probe or relevant local probe.
6. Review the diagnostics page and serial output for a redacted error.
7. Search existing issues for the same device firmware and symptom.

## Information that helps

- Cardputer firmware version and SHA-256 manifest entry;
- target model, hardware revision, product type, and device firmware version;
- router/AP model and whether guest Wi-Fi, mesh, VLAN, or client isolation is enabled;
- exact keys or page sequence used;
- expected and actual behavior;
- frequency and timing of the issue;
- sanitized diagnostics or serial logs;
- result of `python probe.py ...` and its stage/code;
- whether WAN was available and whether LAN control still worked.

For UI problems, a photo is useful after checking that it contains no setup password, IP address, serial number, or credential.

## Never publish

- `.secrets/dyson-local.json`;
- Wi-Fi, Tapo, or MyDyson passwords;
- MyDyson OTP;
- TP09 local MQTT credential;
- unredacted setup pages, HTTP payloads, cookies, or packet captures;
- private information belonging to another person.

## Common categories

| Symptom | First checks |
| --- | --- |
| No L530E found | Same LAN, UDP broadcast, Tapo app provisioning, guest isolation |
| TP09 offline | Product type `438K`, local credential, mDNS, single MQTT client |
| IR does not respond | Aim/range, GPIO 44, RG57A suffix, inferred state, `IR last/max ms` |
| IR responds late | Use RC3 or newer and check serial `latency_ms` for `WARNING` |
| Screen does not wake | Diagnostics must show `IMU ON`; test movement and wake-key consumption |
| Settings disappear | Check schema validation and both transactional NVS slots |

## Scope boundaries

Requests that require Home Assistant, an always-on server, remote internet control, OTA, scenes, schedules, or cross-VLAN routing may be declined because they conflict with the project's current local-only scope.
