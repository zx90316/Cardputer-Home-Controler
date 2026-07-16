# Contributing to Cardputer Home Controller

Thank you for helping improve private, local appliance control on Cardputer Adv.

English · [繁體中文](CONTRIBUTING.zh-TW.md)

## Before you start

This project intentionally prioritizes:

- local-only operation during normal use;
- automatic LAN discovery rather than fixed IP configuration;
- explicit device confirmation instead of optimistic UI success;
- responsive infrared control independent of network traffic;
- redacted diagnostics and strict secret hygiene;
- reproducible PlatformIO and Python environments.

Changes that introduce a mandatory server, Home Assistant dependency, daily cloud control, plaintext credential logging, or hard-coded device addresses are outside the current project direction.

For a bug, use the [bug report form](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=bug_report.yml). For a design proposal or a new device family, open an issue before investing in a large implementation.

## Development setup

### Python probe

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup.ps1
```

Or set up manually:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m pytest -q
```

### Firmware

```powershell
.\.venv\Scripts\python.exe -m pip install -r .\firmware\requirements.txt
.\scripts\build-firmware.ps1 -RunNativeTests
```

Pinned firmware dependencies live in `firmware/platformio.ini`. Do not loosen versions without explaining the compatibility reason and validating a clean build.

## Branches and commits

- Create a focused branch from the current default branch.
- Keep unrelated refactors out of bug-fix pull requests.
- Use concise, imperative commit subjects, for example `Isolate IR dispatch from network polling`.
- Do not rewrite or remove another contributor's work without explaining why.
- Never commit generated firmware builds, `.secrets/`, `probe-report.json`, credentials, device serial numbers, or private LAN details.

## Code guidelines

### C++ firmware

- Keep UI and network operations asynchronous from the user's perspective.
- Access cross-task state only through mutex-protected snapshots.
- Use wrap-safe time helpers for every `millis()` deadline.
- Preserve property-aware command coalescing and one-shot IR ordering.
- Bound retry intervals, payload lengths, response buffers, and blocking operations.
- Treat queue acceptance, IR transmission, MQTT confirmation, and KLAP readback as distinct outcomes.
- Keep serial logs free of Wi-Fi, Tapo, Dyson, cookie, token, and credential values.
- Compile cleanly with the warnings enabled in `firmware/platformio.ini`.

### Python probe

- All live write tests must snapshot state and attempt restoration on success and failure paths.
- Route user-facing failures through stable stage/code diagnostics.
- Keep credentials in hidden input or `.secrets/`; reports must remain redacted.
- Do not claim support for an operation that the underlying library silently ignores.
- Add simulated-device coverage before requiring real hardware.

## Protocol and device changes

Protocol work must document:

1. the device model, hardware revision, firmware version, and product type;
2. the transport and discovery behavior;
3. a public protocol reference or independently observed behavior;
4. authentication, integrity, replay, length, and timeout handling;
5. confirmation semantics and offline recovery;
6. third-party source and license implications.

Do not copy code from a repository whose license is absent or incompatible. Update `THIRD_PARTY.md` whenever a new library or protocol reference is introduced.

## Tests required for a pull request

Run both suites:

```powershell
.\.venv\Scripts\python.exe -m pytest -q
.\scripts\build-firmware.ps1 -RunNativeTests
```

Add tests in proportion to the change:

| Change | Expected evidence |
| --- | --- |
| Parser, crypto, payload or mapping | Fixed native vectors and tamper/error cases |
| Queue, retry, timeout or task behavior | Native state/pressure/wrap tests |
| Python probe | Unit or simulated-device tests, including redaction |
| UI behavior | Key-flow description and target-screen photo when practical |
| Real device support | Redacted probe report and completed acceptance steps |
| Timing-sensitive IR change | Serial `latency_ms` samples under network load |

Do not include credentials or unredacted packet captures in test fixtures.

## Hardware validation

Use [docs/hardware-acceptance.md](docs/hardware-acceptance.md) for target-device testing. A desktop test pass does not prove IR range, display layout, IMU wake, DHCP recovery, or long-run heap stability.

When hardware is unavailable, state that clearly in the pull request and mark the relevant checklist items as pending.

## Pull requests

A good pull request includes:

- the problem and root cause;
- a focused explanation of the solution;
- user-visible behavior before and after;
- commands and results used for verification;
- hardware and firmware versions tested;
- security, migration, and rollback impact;
- documentation and acceptance-checklist updates.

Maintainers may request a smaller scope, additional evidence, or protocol references before merging.

## Security reports

Do not file public issues for vulnerabilities that expose credentials, device access, or exploit details. Follow [SECURITY.md](SECURITY.md).

## Conduct

Participation in this project is governed by [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
