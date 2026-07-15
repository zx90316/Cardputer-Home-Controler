import json

from cardputer_probe.dyson_protocol import (
    command_topic, normalize_sensor_data, parse_message, restorable_state, state_set_payload, status_topic,
)


def test_topics_for_tp09_family():
    assert command_topic("438E", "ABC-TW-12345678") == "438E/ABC-TW-12345678/command"
    assert status_topic("438E", "ABC-TW-12345678") == "438E/ABC-TW-12345678/status/current"
    assert command_topic("438K", "ABC-TW-12345678") == "438K/ABC-TW-12345678/command"


def test_state_set_payload_and_restore_allowlist():
    payload = json.loads(state_set_payload({"fpwr": "ON", "fnsp": "0001"}))
    assert payload["msg"] == "STATE-SET"
    assert payload["mode-reason"] == "LAPP"
    assert payload["data"] == {"fpwr": "ON", "fnsp": "0001"}
    restored = restorable_state({"fpwr": "OFF", "fnsp": "0003", "ercd": "NONE", "pm25": "0004"})
    assert restored == {"fpwr": "OFF", "fnsp": "0003"}


def test_parse_state_unwraps_values():
    kind, state = parse_message(
        b'{"msg":"CURRENT-STATE","product-state":{"fpwr":["changed","ON"],"fnsp":"0004"}}'
    )
    assert kind == "CURRENT-STATE"
    assert state == {"fpwr": "ON", "fnsp": "0004"}


def test_sensor_mapping_includes_tp09_formaldehyde():
    sensors = normalize_sensor_data(
        {"hact": "0047", "pm25": "0003", "hcho": "0001"},
        {"hflr": "0095", "cflr": "0094"},
    )
    assert sensors == {
        "humidity_percent": "0047", "pm2_5": "0003", "formaldehyde": "0001",
        "carbon_filter_percent": "0094", "hepa_filter_percent": "0095",
    }
