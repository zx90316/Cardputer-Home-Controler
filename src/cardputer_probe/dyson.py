"""MyDyson OTP bootstrap followed by direct LAN MQTT validation."""

from __future__ import annotations

import json
import os
from getpass import getpass
from pathlib import Path
from typing import Any

from libdyson_rest import DysonClient

from .dyson_mqtt import DysonMqttProbe, discover_host
from .errors import ProbeStageError, exception_context
from .redaction import fingerprint, mask_serial


def _select_tp09(devices: list[Any]) -> Any:
    candidates = [device for device in devices if device.type == "438E" or str(device.model).upper() == "TP09"]
    if not candidates:
        raise ProbeStageError("dyson_device_selection", "no_tp09_in_account", context={"device_count": len(devices)})
    if len(candidates) == 1:
        return candidates[0]
    print("Found multiple TP09 devices:")
    for index, device in enumerate(candidates, 1):
        print(f"  {index}. {device.name} ({mask_serial(device.serial_number)})")
    while True:
        answer = input("Select device number: ").strip()
        if answer.isdigit() and 1 <= int(answer) <= len(candidates):
            return candidates[int(answer) - 1]


def acquire_local_config(email: str, password: str, country: str, culture: str) -> tuple[Any, str]:
    """Use the cloud once to obtain and decrypt the LAN-only credential."""
    client = DysonClient(email=email, password=password, country=country, culture=culture, debug=False)
    try:
        client.provision()
    except Exception as exc:
        raise ProbeStageError("dyson_provisioning", "provision_failed", context=exception_context(exc)) from exc
    try:
        challenge = client.begin_login()
    except Exception as exc:
        raise ProbeStageError("dyson_begin_login", "login_challenge_failed", context=exception_context(exc)) from exc
    otp_code = getpass("MyDyson OTP: ").strip()
    try:
        # libdyson-rest models challenge_id as UUID, while httpx's JSON encoder
        # requires the API payload value to be a string.
        client.complete_login(str(challenge.challenge_id), otp_code)
    except Exception as exc:
        raise ProbeStageError("dyson_complete_login", "otp_or_login_failed", context=exception_context(exc)) from exc
    try:
        devices = client.get_devices()
    except Exception as exc:
        raise ProbeStageError("dyson_device_list", "device_list_failed", context=exception_context(exc)) from exc
    device = _select_tp09(devices)
    connected = device.connected_configuration
    if connected is None or connected.mqtt is None:
        raise ProbeStageError("dyson_local_config", "mqtt_configuration_missing")
    try:
        credential = client.decrypt_local_credentials(connected.mqtt.local_broker_credentials, device.serial_number)
    except Exception as exc:
        raise ProbeStageError("dyson_local_config", "credential_decryption_failed", context=exception_context(exc)) from exc
    return device, credential


def save_local_config(path: Path, *, host: str, serial: str, product_type: str, credential: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {"host": host, "serial": serial, "product_type": product_type, "credential": credential}
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def probe_tp09_local(
    *, host: str, serial: str, product_type: str, credential: str,
    timeout: int, write_test: bool, metadata: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Validate TP09 directly over LAN using already bootstrapped credentials."""
    report: dict[str, Any] = {
        "read_ok": False, "local_mqtt_ok": False, "write_readback_ok": False, "restore_ok": False,
    }
    report.update({
        "host": host,
        "product_type": product_type,
        "serial": mask_serial(serial),
        "serial_fingerprint": fingerprint(serial),
        "mqtt": {
            "protocol": "MQTTv3.1", "port": 1883,
            "command_topic_shape": f"{product_type}/<serial>/command",
            "status_topic_shape": f"{product_type}/<serial>/status/current",
        },
    })
    if metadata:
        report.update(metadata)
    mqtt_probe = DysonMqttProbe(host, serial, product_type, credential, timeout)
    try:
        try:
            mqtt_probe.connect()
        except Exception as exc:
            raise ProbeStageError("dyson_mqtt_connect", "local_broker_connection_failed", context=exception_context(exc)) from exc
        report["local_mqtt_ok"] = True
        try:
            snapshot = mqtt_probe.read_snapshot()
        except Exception as exc:
            raise ProbeStageError("dyson_mqtt_read", "state_or_environment_timeout", context=exception_context(exc)) from exc
        required_environment = {"hact", "tact", "pm25", "pm10", "va10", "noxl", "hcho"}
        required_product = {"fpwr", "fnsp", "auto", "oson", "osal", "osau", "fdir", "nmod", "rhtm", "cflr", "hflr"}
        presence = {
            **{f"environment.{key}": key in snapshot.environment for key in sorted(required_environment)},
            **{f"product.{key}": key in snapshot.product for key in sorted(required_product)},
            "state.sltm": "sltm" in snapshot.product or "sltm" in snapshot.environment,
        }
        report["read_ok"] = True
        report["capability_ok"] = all(presence.values())
        report["capability_presence"] = presence
        report["state_before"] = mqtt_probe.public_state()
        report["observed_product_fields"] = sorted(snapshot.product)
        report["observed_environment_fields"] = sorted(snapshot.environment)
        if write_test:
            checks, restore_ok = mqtt_probe.write_readback_restore(snapshot)
            report["checks"] = checks
            report["write_readback_ok"] = bool(checks) and all(checks.values())
            report["restore_ok"] = restore_ok
            report["state_after_restore"] = mqtt_probe.public_state()
        else:
            report["write_test"] = "skipped; rerun with --write-test"
    finally:
        mqtt_probe.close()
    return report


def probe_tp09(
    *, email: str, password: str, country: str, culture: str, host: str | None,
    timeout: int, write_test: bool, save_credential: bool, secrets_dir: Path,
) -> tuple[dict[str, Any], list[str]]:
    device, credential = acquire_local_config(email, password, country, culture)
    serial = device.serial_number
    product_type = device.type
    if host:
        local_host = host
    else:
        try:
            local_host = discover_host(serial, timeout=timeout)
        except Exception as exc:
            raise ProbeStageError("dyson_mdns", "tp09_not_discovered", context=exception_context(exc)) from exc
    firmware = device.connected_configuration.firmware
    report = probe_tp09_local(
        host=local_host, serial=serial, product_type=product_type, credential=credential,
        timeout=timeout, write_test=write_test,
        metadata={"model": device.model, "firmware_version": firmware.version},
    )
    if save_credential:
        path = secrets_dir / "dyson-local.json"
        save_local_config(path, host=local_host, serial=serial, product_type=product_type, credential=credential)
        report["local_config_saved"] = str(path)
    return report, [email, password, credential]


def probe_saved_tp09(path: Path, *, timeout: int, write_test: bool) -> tuple[dict[str, Any], list[str]]:
    """Re-run LAN validation without contacting MyDyson."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        host = str(data["host"])
        serial = str(data["serial"])
        product_type = str(data["product_type"])
        credential = str(data["credential"])
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        raise ProbeStageError("dyson_saved_config", "invalid_or_missing_saved_config", context=exception_context(exc)) from exc
    report = probe_tp09_local(
        host=host, serial=serial, product_type=product_type, credential=credential,
        timeout=timeout, write_test=write_test, metadata={"model": "TP09"},
    )
    report["local_config_saved"] = str(path)
    return report, [credential]
