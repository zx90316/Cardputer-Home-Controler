"""Structured, secret-safe errors for interactive diagnostics."""

from __future__ import annotations

from typing import Any


class ProbeStageError(RuntimeError):
    def __init__(self, stage: str, code: str, *, context: dict[str, Any] | None = None):
        super().__init__(code)
        self.stage = stage
        self.code = code
        self.context = context or {}

    def to_report(self) -> dict[str, Any]:
        return {
            "error_type": type(self).__name__,
            "stage": self.stage,
            "code": self.code,
            "context": self.context,
        }


def exception_context(exc: BaseException) -> dict[str, Any]:
    """Extract status/OS error metadata without serializing request bodies."""
    chain: list[str] = []
    context: dict[str, Any] = {}
    current: BaseException | None = exc
    while current is not None and len(chain) < 8:
        chain.append(type(current).__name__)
        response = getattr(current, "response", None)
        if response is not None:
            context["http_status"] = getattr(response, "status_code", None)
        request = getattr(current, "request", None)
        url = getattr(request, "url", None)
        if url is not None:
            context["endpoint_host"] = getattr(url, "host", None)
            context["endpoint_path"] = getattr(url, "path", None)
        if isinstance(current, OSError):
            context["os_error"] = getattr(current, "winerror", None) or current.errno
        current = current.__cause__
    context["cause_chain"] = chain
    return context
