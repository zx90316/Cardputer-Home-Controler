## Summary / 摘要

<!-- What changed and why? 說明修改內容與原因。 -->

## Root cause or user need / 根因或需求

<!-- For bugs, explain the root cause. For features, explain the user problem. -->

## Implementation / 實作

<!-- Describe architecture, protocol, state, migration, and UI decisions. -->

## Verification / 驗證

<!-- Include exact commands and results. Do not attach secrets. -->

- [ ] Python tests pass: `.\.venv\Scripts\python.exe -m pytest -q`
- [ ] Firmware build and native tests pass: `.\scripts\build-firmware.ps1 -RunNativeTests`
- [ ] `git diff --check` passes
- [ ] Relevant hardware acceptance steps were completed, or unavailable hardware is clearly stated

## Device matrix / 設備矩陣

| Device | Hardware / firmware | Tested behavior | Result |
| --- | --- | --- | --- |
| Cardputer Adv | | | |
| RG57A | | | |
| Dyson TP09 | | | |
| Tapo L530E | | | |

## Security and privacy / 安全與隱私

- [ ] No password, OTP, credential, cookie, token, serial number, MAC, or private LAN data is included
- [ ] Logs and reports remain redacted
- [ ] New third-party sources and licenses are documented in `THIRD_PARTY.md`
- [ ] New network input has authentication/integrity, length, timeout, and error handling

## Compatibility and rollback / 相容與回復

<!-- Describe NVS/schema impact, prior-version compatibility, and rollback path. -->

## Documentation / 文件

- [ ] User-facing behavior is documented
- [ ] `CHANGELOG.md` is updated when appropriate
- [ ] `docs/hardware-acceptance.md` is updated when behavior changes
- [ ] English and Traditional Chinese documentation remain aligned when applicable
