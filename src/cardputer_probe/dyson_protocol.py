"""Small, independently testable subset of the Dyson local MQTT protocol."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from typing import Any


def mqtt_time() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def command_topic(product_type: str, serial: str) -> str:
    return f"{product_type}/{serial}/command"


def status_topic(product_type: str, serial: str) -> str:
    return f"{product_type}/{serial}/status/current"


def request_state_payload() -> str:
    return json.dumps({"msg": "REQUEST-CURRENT-STATE", "time": mqtt_time()}, separators=(",", ":"))


def request_environment_payload() -> str:
    return json.dumps(
        {"msg": "REQUEST-PRODUCT-ENVIRONMENT-CURRENT-SENSOR-DATA", "time": mqtt_time()},
        separators=(",", ":"),
    )


def state_set_payload(data: dict[str, str]) -> str:
    return json.dumps(
        {"msg": "STATE-SET", "time": mqtt_time(), "mode-reason": "LAPP", "data": data},
        separators=(",", ":"),
    )


def unwrap(value: Any) -> Any:
    """Dyson occasionally reports fields as [metadata, actual_value]."""
    return value[1] if isinstance(value, list) and len(value) > 1 else value


def parse_message(payload: bytes | str) -> tuple[str, dict[str, Any]]:
    raw = payload.decode("utf-8") if isinstance(payload, bytes) else payload
    message = json.loads(raw)
    kind = str(message.get("msg", "UNKNOWN"))
    if kind in {"CURRENT-STATE", "STATE-CHANGE"}:
        state = message.get("product-state", {})
    elif kind == "ENVIRONMENTAL-CURRENT-SENSOR-DATA":
        state = message.get("data", {})
    else:
        state = {}
    return kind, {str(key): unwrap(value) for key, value in state.items()}


WRITABLE_FIELDS = {"fpwr", "fnsp", "auto", "oson", "ancp", "osal", "osau", "fdir", "nmod", "rhtm", "sltm"}


def restorable_state(state: dict[str, Any]) -> dict[str, str]:
    """Select only known writable fields; never echo sensor/error fields as commands."""
    return {key: str(state[key]) for key in WRITABLE_FIELDS if key in state}


def normalize_sensor_data(environment: dict[str, Any], product_state: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    mapping = {
        "hact": "humidity_percent",
        "tact": "temperature_kelvin_tenths",
        "pm25": "pm2_5",
        "pm10": "pm10",
        "va10": "voc",
        "noxl": "no2",
        "hcho": "formaldehyde",
        "sltm": "sleep_timer_minutes",
    }
    for raw, friendly in mapping.items():
        source = environment if raw in environment else product_state
        if raw in source:
            result[friendly] = source[raw]
    for raw, friendly in {"cflr": "carbon_filter_percent", "hflr": "hepa_filter_percent"}.items():
        if raw in product_state:
            result[friendly] = product_state[raw]
    return result
