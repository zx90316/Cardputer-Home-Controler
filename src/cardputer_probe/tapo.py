"""Tapo L530E discovery and reversible live probe."""

from __future__ import annotations

from dataclasses import asdict
from ipaddress import IPv4Address, ip_interface
from typing import Any

import ifaddr
from kasa import Credentials, Discover, Module
from kasa.interfaces import LightState

from .errors import ProbeStageError


def _enum_value(value: Any) -> Any:
    return getattr(value, "value", value)


async def _refresh_and_check(device, predicate) -> bool:
    await device.update()
    return bool(predicate())


def lan_broadcast_targets() -> list[str]:
    """Return directed broadcasts, excluding loopback, link-local and /32 tunnels."""
    targets: set[str] = set()
    for adapter in ifaddr.get_adapters():
        for address in adapter.ips:
            if not isinstance(address.ip, str):
                continue
            ip = IPv4Address(address.ip)
            prefix = address.network_prefix
            if ip.is_loopback or ip.is_link_local or prefix >= 31:
                continue
            targets.add(str(ip_interface(f"{ip}/{prefix}").network.broadcast_address))
    return sorted(targets)


async def discover_l530e(username: str, password: str, host: str | None = None, timeout: int = 8):
    credentials = Credentials(username, password)
    raw: list[dict[str, Any]] = []

    def on_raw(item: dict[str, Any]) -> None:
        response = item["discovery_response"]
        model = response.get("device_model") or response.get("model") or response.get("result", {}).get("device_model")
        device_type = response.get("device_type") or response.get("result", {}).get("device_type")
        raw.append({"host": item["meta"]["ip"], "model": model, "device_type": device_type})

    if host:
        device = await Discover.discover_single(
            host, credentials=credentials, discovery_timeout=timeout, timeout=timeout, on_discovered_raw=on_raw
        )
        devices = [device] if device is not None else []
        targets = [host]
    else:
        targets = lan_broadcast_targets() or ["255.255.255.255"]
        discovered: dict[str, Any] = {}
        for target in targets:
            discovered.update(
                await Discover.discover(
                    target=target, credentials=credentials, discovery_timeout=timeout,
                    timeout=timeout, on_discovered_raw=on_raw,
                )
            )
        devices = list(discovered.values())
    matches = []
    update_errors: list[str] = []
    for device in devices:
        try:
            await device.update()
            if str(device.model).upper().startswith("L530"):
                matches.append(device)
            else:
                await device.disconnect()
        except Exception as exc:
            update_errors.append(type(exc).__name__)
            await device.disconnect()
    if not matches:
        l530_hosts = sorted({str(item["host"]) for item in raw if str(item.get("model", "")).upper().startswith("L530")})
        stage = "tapo_authentication" if l530_hosts else "tapo_discovery"
        raise ProbeStageError(
            stage,
            "l530e_seen_but_auth_failed" if l530_hosts else "no_l530e_discovery_response",
            context={"broadcast_targets": targets, "candidate_hosts": l530_hosts, "update_error_types": sorted(set(update_errors))},
        )
    if len(matches) > 1 and not host:
        candidate_hosts = sorted(device.host for device in matches)
        for device in matches:
            await device.disconnect()
        raise ProbeStageError(
            "tapo_selection", "multiple_l530e_devices",
            context={"candidate_hosts": candidate_hosts, "hint": "rerun_with_tapo_host"},
        )
    return matches[0]


async def probe_l530e(username: str, password: str, host: str | None, timeout: int, write_test: bool) -> dict[str, Any]:
    device = await discover_l530e(username, password, host, timeout)
    report: dict[str, Any] = {"read_ok": False, "write_readback_ok": False, "restore_ok": False}
    light = None
    snapshot = None
    effect_module = None
    original_effect = None
    try:
        await device.update()
        light = device.modules[Module.Light]
        snapshot = LightState(light_on=bool(device.is_on))
        if light.has_feature("brightness"):
            snapshot.brightness = light.brightness
        if light.has_feature("hsv"):
            snapshot.hue = light.hsv.hue
            snapshot.saturation = light.hsv.saturation
        if light.has_feature("color_temp"):
            snapshot.color_temp = light.color_temp
        connection = device.config.connection_type
        transport = getattr(device.protocol, "_transport", None)
        report.update(
            {
                "host": device.host,
                "model": device.model,
                "hardware_version": getattr(device, "hw_info", {}).get("hw_ver"),
                "firmware_version": getattr(device, "hw_info", {}).get("sw_ver"),
                "protocol": {
                    "family": _enum_value(connection.device_family),
                    "encryption": _enum_value(connection.encryption_type),
                    "login_version": connection.login_version,
                    "protocol_class": type(device.protocol).__name__,
                    "transport_class": type(transport).__name__ if transport else None,
                },
                "capabilities": {
                    "power": True,
                    "brightness": light.has_feature("brightness"),
                    "hsv": light.has_feature("hsv"),
                    "color_temp": light.has_feature("color_temp"),
                    "transition": False,
                    "effects": Module.LightEffect in device.modules,
                    "presets": Module.LightPreset in device.modules,
                },
                "state_before": asdict(snapshot),
                "read_ok": True,
            }
        )
        if Module.LightEffect in device.modules:
            effect_module = device.modules[Module.LightEffect]
            original_effect = effect_module.effect
            report["effect_list"] = list(effect_module.effect_list)
        if Module.LightPreset in device.modules:
            report["preset_list"] = list(device.modules[Module.LightPreset].preset_list)
        if not write_test:
            report["write_test"] = "skipped; rerun with --write-test"
            return report

        checks: dict[str, bool] = {
            "power": False, "brightness": False, "color_temp": False,
            "hsv": False, "effects": False, "presets": False,
        }
        await device.turn_on()
        checks["power"] = await _refresh_and_check(device, lambda: light.state.light_on is True)
        if light.has_feature("brightness"):
            value = 35 if snapshot.brightness != 35 else 55
            await light.set_brightness(value)
            checks["brightness"] = await _refresh_and_check(device, lambda: light.state.brightness == value)
        if light.has_feature("color_temp"):
            await light.set_color_temp(4000)
            checks["color_temp"] = await _refresh_and_check(device, lambda: light.state.color_temp == 4000)
        if light.has_feature("hsv"):
            await light.set_hsv(210, 70, 45)
            checks["hsv"] = await _refresh_and_check(
                device,
                lambda: light.state.hue == 210 and light.state.saturation == 70 and light.state.brightness == 45,
            )
        if effect_module:
            candidates = [item for item in effect_module.effect_list if item != effect_module.LIGHT_EFFECTS_OFF]
            if candidates:
                effect_id = effect_module._scenes_names_to_id[candidates[0]]
                effect_brightness = effect_module._get_effect_data(effect_id)["color_status_list"][0][0]
                await effect_module.set_effect(candidates[0], brightness=effect_brightness)
                await device.update()
                checks["effects"] = effect_module.effect == candidates[0]
        if Module.LightPreset in device.modules:
            presets = device.modules[Module.LightPreset]
            candidates = [item for item in presets.preset_list if item != presets.PRESET_NOT_SET]
            if candidates:
                await presets.set_preset(candidates[0])
                await device.update()
                checks["presets"] = presets.preset == candidates[0]
        report["checks"] = checks
        report["write_readback_ok"] = bool(checks) and all(checks.values())
    except Exception as exc:
        if not report["read_ok"]:
            raise
        report["probe_error_type"] = type(exc).__name__
        report["write_readback_ok"] = False
    finally:
        if write_test and light is not None and snapshot is not None:
            try:
                if effect_module and original_effect is not None:
                    await effect_module.set_effect(effect_module.LIGHT_EFFECTS_OFF)
                restore_state = LightState(**asdict(snapshot))
                if snapshot.light_on is False:
                    restore_state.light_on = True
                await light.set_state(restore_state)
                await device.update()
                restored = True
                for field in ("brightness", "hue", "saturation", "color_temp"):
                    expected = getattr(snapshot, field)
                    if expected is not None:
                        restored = restored and getattr(light.state, field) == expected
                if snapshot.light_on is False:
                    await device.turn_off()
                if effect_module and original_effect not in {None, effect_module.LIGHT_EFFECTS_OFF}:
                    await effect_module.set_effect(original_effect)
                await device.update()
                restored = restored and light.state.light_on == snapshot.light_on
                if effect_module and original_effect is not None:
                    restored = restored and effect_module.effect == original_effect
                report["restore_ok"] = bool(restored)
                report["state_after_restore"] = asdict(light.state)
            except Exception as exc:
                report["restore_error"] = f"{type(exc).__name__}: {exc}"
        await device.disconnect()
    return report
