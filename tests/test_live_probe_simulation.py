import asyncio
import json
from types import SimpleNamespace

from kasa import Module
from kasa.interfaces import LightState

from cardputer_probe.dyson_mqtt import DysonMqttProbe, DysonSnapshot
from cardputer_probe.tapo import probe_l530e


class FakeLight:
    def __init__(self):
        self.state = LightState(light_on=False, brightness=72, hue=15, saturation=40, color_temp=3100)

    def has_feature(self, name):
        return name in {"brightness", "hsv", "color_temp"}

    @property
    def brightness(self):
        return self.state.brightness

    @property
    def color_temp(self):
        return self.state.color_temp

    @property
    def hsv(self):
        return SimpleNamespace(hue=self.state.hue, saturation=self.state.saturation, value=self.state.brightness)

    async def set_brightness(self, value):
        self.state.brightness = value

    async def set_color_temp(self, value):
        self.state.color_temp = value

    async def set_hsv(self, hue, saturation, value):
        self.state.hue, self.state.saturation, self.state.brightness = hue, saturation, value

    async def set_state(self, state):
        self.state = LightState(**vars(state))


class FakeTapo:
    def __init__(self):
        self.host, self.model = "192.0.2.10", "L530E"
        self.hw_info = {"hw_ver": "1.0", "sw_ver": "1.2.3"}
        self.light = FakeLight()
        self.modules = {Module.Light: self.light}
        connection = SimpleNamespace(device_family=SimpleNamespace(value="SMART.TAPOBULB"), encryption_type=SimpleNamespace(value="KLAP"), login_version=2)
        self.config = SimpleNamespace(connection_type=connection)
        self.protocol = SimpleNamespace(_transport=SimpleNamespace())
        self.disconnected = False

    @property
    def is_on(self):
        return self.light.state.light_on

    async def update(self):
        return None

    async def turn_on(self):
        self.light.state.light_on = True

    async def turn_off(self):
        self.light.state.light_on = False

    async def disconnect(self):
        self.disconnected = True


def test_tapo_snapshot_restored_when_capabilities_are_exercised(monkeypatch):
    device = FakeTapo()

    async def fake_discover(*args, **kwargs):
        return device

    monkeypatch.setattr("cardputer_probe.tapo.discover_l530e", fake_discover)
    report = asyncio.run(probe_l530e("user", "pass", None, 1, True))
    assert report["write_readback_ok"] is False  # fake intentionally has no effects/presets
    assert report["restore_ok"] is True
    assert device.light.state == LightState(light_on=False, brightness=72, hue=15, saturation=40, color_temp=3100)
    assert device.disconnected is True


def test_dyson_full_write_matrix_restores_snapshot():
    product = {
        "fpwr": "OFF", "auto": "ON", "fnsp": "AUTO", "oson": "OIOF", "ancp": "CUST",
        "osal": "0090", "osau": "0270", "fdir": "ON", "nmod": "OFF", "rhtm": "ON",
    }
    environment = {"sltm": "OFF"}
    probe = DysonMqttProbe("192.0.2.20", "ABC-TW-12345678", "438E", "credential", timeout=1)
    probe.product_state = dict(product)
    probe.environment = dict(environment)

    def fake_publish(payload):
        data = json.loads(payload).get("data", {})
        for key, value in data.items():
            if key == "sltm":
                probe.environment[key] = value
            else:
                probe.product_state[key] = value

    probe.publish = fake_publish
    probe.wait_for = lambda predicate, timeout=None: predicate()
    checks, restored = probe.write_readback_restore(DysonSnapshot(product, environment))
    assert all(checks.values())
    assert restored is True
    assert probe.product_state == product
    assert probe.environment == environment


def test_dyson_restores_angles_before_disabling_oscillation():
    product = {
        "fpwr": "ON", "auto": "OFF", "fnsp": "0004", "oson": "OFF", "ancp": "0045",
        "osal": "0157", "osau": "0202", "fdir": "ON", "nmod": "OFF", "rhtm": "ON", "sltm": "OFF",
    }
    probe = DysonMqttProbe("192.0.2.20", "ABC-TW-12345678", "438K", "credential", timeout=1)
    probe.product_state = {**product, "osal": "0045", "osau": "0315"}
    published = []

    def fake_publish(payload):
        data = json.loads(payload)["data"]
        published.append(data)
        probe.product_state.update(data)

    probe.publish = fake_publish
    probe.wait_for = lambda predicate, timeout=None: predicate()
    assert probe.restore_snapshot(DysonSnapshot(product, {})) is True
    angle_index = next(i for i, item in enumerate(published) if "osal" in item)
    off_index = next(i for i, item in enumerate(published) if item == {"oson": "OFF"})
    assert published[angle_index]["oson"] == "ON"
    assert angle_index < off_index
    assert probe.product_state["osal"] == "0157"
    assert probe.product_state["osau"] == "0202"
