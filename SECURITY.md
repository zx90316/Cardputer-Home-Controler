# Security Policy

English · [繁體中文](SECURITY.zh-TW.md)

## Supported versions

Security fixes are developed against the latest release candidate and the default branch.

| Version | Supported |
| --- | --- |
| `1.0.0-rc3` | Yes |
| Earlier release candidates | No; upgrade before reporting |
| Unreleased default branch | Best effort |

## Reporting a vulnerability

Do **not** disclose vulnerabilities, credentials, device serial numbers, private IP addresses, packet captures containing secrets, or working exploit details in a public issue.

Preferred reporting path:

1. Open this repository's **Security** tab and use **Report a vulnerability** if private vulnerability reporting is available.
2. If that option is unavailable, contact the repository owner privately through a contact method published on the [maintainer's GitHub profile](https://github.com/zx90316).

Include only the minimum information needed to reproduce the issue:

- affected version or commit;
- affected device and firmware version;
- impact and realistic attack prerequisites;
- reproducible steps or a minimal proof of concept;
- whether physical access, LAN access, or cloud access is required;
- suggested mitigation, if known;
- redacted logs or captures.

Never send real Wi-Fi passwords, Tapo passwords, MyDyson passwords/OTP, TP09 MQTT credentials, KLAP session material, or unredacted `.secrets/` files.

## Response process

The maintainer will handle reports on a best-effort basis:

1. confirm receipt and establish a private communication path;
2. reproduce and assess severity;
3. prepare a fix and regression coverage;
4. coordinate disclosure and release timing;
5. credit the reporter if requested and appropriate.

Please allow reasonable time for investigation before public disclosure, especially when hardware reproduction is required.

## Security model and accepted boundaries

The following are documented design boundaries, not automatically vulnerabilities:

- credentials are stored in ordinary, unencrypted ESP32 NVS;
- physical flash access may recover stored configuration;
- the setup AP is a temporary local provisioning channel;
- devices are expected to share a trusted, reachable IPv4 LAN;
- RG57A infrared state is inferred and has no authenticity or readback channel;
- MyDyson cloud access is used once to bootstrap the TP09 local credential;
- the project does not provide internet-facing control, OTA, or a remote Web API.

A report is still welcome if an implementation flaw exceeds these documented boundaries—for example, secrets appearing in logs, setup access persisting unexpectedly, authentication bypass, unsafe response parsing, or control from outside the intended LAN.

## If a secret was exposed

1. Remove the data from public view; assume Git history and caches may retain it.
2. Change the Wi-Fi or Tapo password if exposed.
3. Re-provision or rotate the TP09 local credential when possible.
4. Erase and reconfigure the Cardputer if flash contents may have been copied.
5. Do not attach the replacement credential to an issue or pull request.
