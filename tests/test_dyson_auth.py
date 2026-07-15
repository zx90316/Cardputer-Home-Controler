from types import SimpleNamespace
from uuid import UUID

from cardputer_probe.dyson import acquire_local_config


def test_challenge_uuid_is_serialized_before_complete_login(monkeypatch):
    challenge = UUID("12345678-1234-5678-1234-567812345678")
    mqtt = SimpleNamespace(local_broker_credentials="encrypted")
    device = SimpleNamespace(
        type="438E",
        model="TP09",
        serial_number="ABC-TW-12345678",
        connected_configuration=SimpleNamespace(mqtt=mqtt),
    )

    class FakeClient:
        def __init__(self, **kwargs):
            pass

        def provision(self):
            return "version"

        def begin_login(self):
            return SimpleNamespace(challenge_id=challenge)

        def complete_login(self, challenge_id, otp):
            assert challenge_id == str(challenge)
            assert isinstance(challenge_id, str)

        def get_devices(self):
            return [device]

        def decrypt_local_credentials(self, encrypted, serial):
            return "local-credential"

    monkeypatch.setattr("cardputer_probe.dyson.DysonClient", FakeClient)
    monkeypatch.setattr("cardputer_probe.dyson.getpass", lambda prompt: "123456")
    selected, credential = acquire_local_config("user@example.test", "password", "TW", "zh-TW")
    assert selected is device
    assert credential == "local-credential"
