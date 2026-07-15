"""Secret-safe serialization helpers used by reports and logs."""

from __future__ import annotations

import hashlib
from collections.abc import Mapping, Sequence
from typing import Any

SECRET_FRAGMENTS = (
    "password",
    "credential",
    "secret",
    "token",
    "authorization",
    "cookie",
    "email",
    "username",
)


def fingerprint(value: str, length: int = 12) -> str:
    """Return a stable non-reversible identifier suitable for diagnostics."""
    return hashlib.sha256(value.encode("utf-8")).hexdigest()[:length]


def mask_serial(value: str) -> str:
    """Keep only enough of a serial number to identify a physical unit locally."""
    if len(value) <= 4:
        return "****"
    return f"***{value[-4:]}"


def redact(value: Any, *, key: str = "") -> Any:
    """Recursively remove values whose key may contain a secret."""
    lowered = key.casefold().replace("-", "_")
    if any(fragment in lowered for fragment in SECRET_FRAGMENTS):
        return "<redacted>"
    if lowered in {"serial", "serial_number", "serialnumber"} and value is not None:
        return mask_serial(str(value))
    if isinstance(value, Mapping):
        return {str(k): redact(v, key=str(k)) for k, v in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [redact(item) for item in value]
    return value


def assert_secret_free(value: Any, forbidden_values: Sequence[str]) -> None:
    """Fail closed before writing a report if a supplied secret is still present."""
    rendered = repr(value)
    leaked = [item for item in forbidden_values if item and item in rendered]
    if leaked:
        raise ValueError("Refusing to write report: sensitive value was not redacted")


def scrub_values(value: Any, forbidden_values: Sequence[str]) -> Any:
    """Remove exact known secrets even when a third-party exception embeds one."""
    secrets = [item for item in forbidden_values if item]
    if isinstance(value, Mapping):
        return {str(k): scrub_values(v, secrets) for k, v in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [scrub_values(item, secrets) for item in value]
    if isinstance(value, str):
        for secret in secrets:
            value = value.replace(secret, "<redacted>")
    return value
