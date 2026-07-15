import json

import pytest

from cardputer_probe.redaction import assert_secret_free, mask_serial, redact, scrub_values


def test_recursive_redaction_and_serial_masking():
    source = {
        "password": "super-secret",
        "nested": {"localBrokerCredentials": "mqtt-secret", "serial_number": "ABC-TW-12345678"},
        "safe": [1, 2],
    }
    result = redact(source)
    rendered = json.dumps(result)
    assert "super-secret" not in rendered
    assert "mqtt-secret" not in rendered
    assert result["nested"]["serial_number"] == "***5678"
    assert result["safe"] == [1, 2]


def test_secret_free_fails_closed():
    with pytest.raises(ValueError):
        assert_secret_free({"oops": "known-value"}, ["known-value"])


def test_short_serial_is_fully_masked():
    assert mask_serial("1234") == "****"


def test_known_secret_is_scrubbed_from_exception_text():
    result = scrub_values({"error": "login failed for user@example.test"}, ["user@example.test"])
    assert result == {"error": "login failed for <redacted>"}
