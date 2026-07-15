"""Probe report construction and safe persistence."""

from __future__ import annotations

import json
import platform
from datetime import UTC, datetime
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
from typing import Any

from .redaction import assert_secret_free, redact, scrub_values


def _package_version(name: str) -> str:
    try:
        return version(name)
    except PackageNotFoundError:
        return "not-installed"


def new_report() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "generated_at": datetime.now(UTC).isoformat(),
        "host": {"python": platform.python_version(), "platform": platform.system()},
        "dependencies": {
            "python-kasa": _package_version("python-kasa"),
            "libdyson-rest": _package_version("libdyson-rest"),
            "paho-mqtt": _package_version("paho-mqtt"),
            "zeroconf": _package_version("zeroconf"),
            "ifaddr": _package_version("ifaddr"),
        },
        "devices": {},
        "gate": {"passed": False, "reason": "Both live write/readback/restore probes are required"},
    }


def merge_existing_devices(path: Path, report: dict[str, Any]) -> None:
    """Preserve the other device result when tapo/dyson are probed separately."""
    if not path.exists():
        return
    try:
        existing = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    if existing.get("schema_version") != report.get("schema_version"):
        return
    generated_at = existing.get("generated_at")
    for key, value in existing.get("devices", {}).items():
        if isinstance(value, dict):
            copied = dict(value)
            copied.setdefault("tested_at", generated_at)
            report["devices"][key] = copied


def evaluate_gate(report: dict[str, Any]) -> None:
    devices = report.get("devices", {})
    tapo = devices.get("tapo_l530e", {})
    dyson = devices.get("dyson_tp09", {})
    required = {
        "tapo_read": tapo.get("read_ok") is True,
        "tapo_write_readback": tapo.get("write_readback_ok") is True,
        "tapo_restore": tapo.get("restore_ok") is True,
        "tapo_klap": tapo.get("protocol", {}).get("encryption") == "KLAP",
        "dyson_read": dyson.get("read_ok") is True,
        "dyson_capabilities": dyson.get("capability_ok") is True,
        "dyson_write_readback": dyson.get("write_readback_ok") is True,
        "dyson_restore": dyson.get("restore_ok") is True,
        "dyson_local_mqtt": dyson.get("local_mqtt_ok") is True,
    }
    report["gate"] = {
        "passed": all(required.values()),
        "checks": required,
        "reason": "ready_for_firmware" if all(required.values()) else "live_probe_incomplete_or_failed",
    }


def write_report(path: Path, report: dict[str, Any], forbidden_values: list[str]) -> None:
    safe = scrub_values(redact(report), forbidden_values)
    assert_secret_free(safe, forbidden_values)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(safe, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
