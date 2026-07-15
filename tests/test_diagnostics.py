from types import SimpleNamespace

from cardputer_probe.cli import _error_record, _error_summary
from cardputer_probe.errors import ProbeStageError
from cardputer_probe.tapo import lan_broadcast_targets


def test_structured_error_is_safe_and_actionable():
    error = ProbeStageError(
        "tapo_selection",
        "multiple_l530e_devices",
        context={"candidate_hosts": ["192.168.68.61", "192.168.68.62"]},
    )
    assert _error_summary(error) == "tapo_selection/multiple_l530e_devices"
    assert _error_record(error)["context"]["candidate_hosts"] == ["192.168.68.61", "192.168.68.62"]


def test_directed_broadcast_ignores_tunnel_and_link_local(monkeypatch):
    adapters = [
        SimpleNamespace(ips=[SimpleNamespace(ip="192.168.68.69", network_prefix=22)]),
        SimpleNamespace(ips=[SimpleNamespace(ip="100.85.116.40", network_prefix=32)]),
        SimpleNamespace(ips=[SimpleNamespace(ip="169.254.1.2", network_prefix=16)]),
    ]
    monkeypatch.setattr("cardputer_probe.tapo.ifaddr.get_adapters", lambda: adapters)
    assert lan_broadcast_targets() == ["192.168.71.255"]
