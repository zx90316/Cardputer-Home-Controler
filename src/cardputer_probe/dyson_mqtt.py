"""Direct LAN MQTT probe for Dyson fans."""

from __future__ import annotations

import json
import queue
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any

import paho.mqtt.client as mqtt
from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

from .dyson_protocol import (
    command_topic,
    normalize_sensor_data,
    parse_message,
    request_environment_payload,
    request_state_payload,
    restorable_state,
    state_set_payload,
    status_topic,
)

DYSON_SERVICE = "_dyson_mqtt._tcp.local."


class _Listener(ServiceListener):
    def __init__(self, serial: str, found: queue.Queue[str]):
        self.serial = serial
        self.found = found

    def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        info = zc.get_service_info(type_, name, timeout=3000)
        if info is None:
            return
        advertised_serial = name.split(".", 1)[0].split("_", 1)[-1]
        if advertised_serial.casefold() != self.serial.casefold():
            return
        for packed in info.addresses:
            if len(packed) == 4:
                try:
                    self.found.put_nowait(socket.inet_ntoa(packed))
                except queue.Full:
                    pass
                return

    def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        return

    def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        self.add_service(zc, type_, name)


def discover_host(serial: str, timeout: float = 8.0) -> str:
    found: queue.Queue[str] = queue.Queue(maxsize=1)
    zc = Zeroconf(ip_version=None)
    browser = ServiceBrowser(zc, DYSON_SERVICE, _Listener(serial, found))
    try:
        return found.get(timeout=timeout)
    except queue.Empty as exc:
        raise TimeoutError("TP09 was not discovered by mDNS; use --dyson-host") from exc
    finally:
        browser.cancel()
        zc.close()


@dataclass
class DysonSnapshot:
    product: dict[str, Any]
    environment: dict[str, Any]


class DysonMqttProbe:
    def __init__(self, host: str, serial: str, product_type: str, credential: str, timeout: float = 10):
        self.host = host
        self.serial = serial
        self.product_type = product_type
        self.credential = credential
        self.timeout = timeout
        self.connected = threading.Event()
        self.messages = threading.Condition()
        self.product_state: dict[str, Any] = {}
        self.environment: dict[str, Any] = {}
        self.last_error: str | None = None
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv31)
        self.client.username_pw_set(serial, credential)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        if reason_code == 0:
            client.subscribe(status_topic(self.product_type, self.serial), qos=1)
            self.connected.set()
        else:
            self.last_error = f"MQTT CONNACK {reason_code}"
            self.connected.set()

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        if reason_code != 0:
            self.last_error = f"Unexpected MQTT disconnect {reason_code}"

    def _on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        try:
            kind, data = parse_message(msg.payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            self.last_error = f"Invalid MQTT JSON: {type(exc).__name__}"
            return
        with self.messages:
            if kind in {"CURRENT-STATE", "STATE-CHANGE"}:
                self.product_state.update(data)
            elif kind == "ENVIRONMENTAL-CURRENT-SENSOR-DATA":
                self.environment.update(data)
            self.messages.notify_all()

    def connect(self) -> None:
        self.client.connect(self.host, 1883, keepalive=60)
        self.client.loop_start()
        if not self.connected.wait(self.timeout):
            self.close()
            raise TimeoutError("Timed out connecting to TP09 local MQTT broker")
        if self.last_error:
            self.close()
            raise ConnectionError(self.last_error)

    def close(self) -> None:
        try:
            self.client.disconnect()
        finally:
            self.client.loop_stop()

    def publish(self, payload: str) -> None:
        info = self.client.publish(command_topic(self.product_type, self.serial), payload, qos=1)
        info.wait_for_publish(timeout=self.timeout)

    def wait_for(self, predicate, timeout: float | None = None) -> bool:
        deadline = time.monotonic() + (timeout or self.timeout)
        with self.messages:
            while not predicate():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self.messages.wait(remaining)
        return True

    def read_snapshot(self) -> DysonSnapshot:
        self.publish(request_state_payload())
        self.publish(request_environment_payload())
        ok = self.wait_for(lambda: bool(self.product_state) and bool(self.environment))
        if not ok:
            raise TimeoutError("TP09 did not return both product and environmental state")
        return DysonSnapshot(dict(self.product_state), dict(self.environment))

    def write_readback_restore(self, snapshot: DysonSnapshot) -> tuple[dict[str, bool], bool]:
        original_speed = str(snapshot.product.get("fnsp", "0001"))
        test_speed = "0002" if original_speed == "0001" else "0001"
        checks: dict[str, bool] = {}
        restore_ok = False

        def current(key: str) -> Any:
            return self.product_state[key] if key in self.product_state else self.environment.get(key)

        def check(name: str, data: dict[str, str], expected: dict[str, str]) -> None:
            self.publish(state_set_payload(data))
            checks[name] = self.wait_for(
                lambda: all(str(current(key)) == value for key, value in expected.items())
            )

        try:
            check("power_speed", {"fpwr": "ON", "auto": "OFF", "fnsp": test_speed}, {"fpwr": "ON", "auto": "OFF", "fnsp": test_speed})
            check("auto_mode", {"auto": "ON"}, {"auto": "ON"})

            current_oson = str(snapshot.product.get("oson", "OFF"))
            enabled_oson = "OION" if current_oson in {"OION", "OIOF"} else "ON"
            check(
                "oscillation_angle",
                {"fpwr": "ON", "oson": enabled_oson, "ancp": "CUST", "osal": "0045", "osau": "0315"},
                {"oson": enabled_oson, "osal": "0045", "osau": "0315"},
            )
            check("airflow_direction", {"fdir": "OFF" if snapshot.product.get("fdir") == "ON" else "ON"},
                  {"fdir": "OFF" if snapshot.product.get("fdir") == "ON" else "ON"})
            check("night_mode", {"nmod": "OFF" if snapshot.product.get("nmod") == "ON" else "ON"},
                  {"nmod": "OFF" if snapshot.product.get("nmod") == "ON" else "ON"})
            check("continuous_monitoring", {"rhtm": "OFF" if snapshot.product.get("rhtm") == "ON" else "ON"},
                  {"rhtm": "OFF" if snapshot.product.get("rhtm") == "ON" else "ON"})
            check("sleep_timer", {"sltm": "0015"}, {"sltm": "0015"})
        except Exception as exc:
            self.last_error = f"Write test failed: {type(exc).__name__}"
            checks["probe_error"] = False
        finally:
            try:
                restore_ok = self.restore_snapshot(snapshot)
            except Exception as exc:
                self.last_error = f"Restore failed: {type(exc).__name__}"
        return checks, restore_ok

    def restore_snapshot(self, snapshot: DysonSnapshot) -> bool:
        """Restore changed controls in an order accepted by TP09/438K firmware."""
        expected = {
            key: str(snapshot.product[key])
            for key in ("fpwr", "auto", "fnsp", "oson", "ancp", "osal", "osau", "fdir", "nmod", "rhtm", "sltm")
            if key in snapshot.product
        }
        if "sltm" not in expected and "sltm" in snapshot.environment:
            expected["sltm"] = str(snapshot.environment["sltm"])

        def current(key: str) -> Any:
            return self.product_state[key] if key in self.product_state else self.environment.get(key)

        def apply(data: dict[str, str], verify: dict[str, str] | None = None) -> bool:
            if not data:
                return True
            self.publish(state_set_payload(data))
            wanted = verify or data
            return self.wait_for(
                lambda: all(str(current(key)) == value for key, value in wanted.items()),
                timeout=max(self.timeout, 15),
            )

        ok = True
        ok = apply({key: expected[key] for key in ("fdir", "nmod", "rhtm", "sltm") if key in expected}) and ok

        angle_fields = {key: expected[key] for key in ("ancp", "osal", "osau") if key in expected}
        if angle_fields:
            original_oson = expected.get("oson", "OFF")
            enabled_oson = "OION" if original_oson in {"OION", "OIOF"} else "ON"
            angle_command = {"fpwr": "ON", "oson": enabled_oson, **angle_fields}
            ok = apply(angle_command, angle_fields) and ok
            if "oson" in expected:
                ok = apply({"oson": original_oson}) and ok

        ok = apply({key: expected[key] for key in ("auto", "fnsp") if key in expected}) and ok
        if "fpwr" in expected:
            ok = apply({"fpwr": expected["fpwr"]}) and ok

        final_ok = self.wait_for(
            lambda: all(str(current(key)) == value for key, value in expected.items()),
            timeout=max(self.timeout, 15),
        )
        return ok and final_ok

    def public_state(self) -> dict[str, Any]:
        controls = {key: self.product_state[key] for key in sorted(restorable_state(self.product_state))}
        return {"controls": controls, "sensors": normalize_sensor_data(self.environment, self.product_state)}
