import json

from cardputer_probe.report import evaluate_gate, merge_existing_devices, new_report, write_report


def test_gate_requires_both_devices_and_klap(tmp_path):
    report = new_report()
    report["devices"] = {
        "tapo_l530e": {"read_ok": True, "write_readback_ok": True, "restore_ok": True, "protocol": {"encryption": "KLAP"}},
        "dyson_tp09": {"read_ok": True, "capability_ok": True, "write_readback_ok": True, "restore_ok": True, "local_mqtt_ok": True},
    }
    evaluate_gate(report)
    assert report["gate"]["passed"] is True
    path = tmp_path / "report.json"
    write_report(path, report, ["not-present-secret"])
    assert json.loads(path.read_text(encoding="utf-8"))["gate"]["passed"] is True


def test_gate_rejects_read_only_probe():
    report = new_report()
    report["devices"] = {"tapo_l530e": {"read_ok": True}, "dyson_tp09": {"read_ok": True}}
    evaluate_gate(report)
    assert report["gate"]["passed"] is False


def test_separate_probe_merges_existing_device(tmp_path):
    path = tmp_path / "probe-report.json"
    path.write_text(
        json.dumps({"schema_version": 1, "generated_at": "old-time", "devices": {"tapo_l530e": {"read_ok": True}}}),
        encoding="utf-8",
    )
    report = new_report()
    merge_existing_devices(path, report)
    assert report["devices"]["tapo_l530e"]["read_ok"] is True
    assert report["devices"]["tapo_l530e"]["tested_at"] == "old-time"
