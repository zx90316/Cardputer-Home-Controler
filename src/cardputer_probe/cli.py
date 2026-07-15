"""Interactive entry point for real-device feasibility validation."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from getpass import getpass
from pathlib import Path
from typing import Any

from .dyson import probe_saved_tp09, probe_tp09
from .errors import ProbeStageError
from .report import evaluate_gate, merge_existing_devices, new_report, write_report
from .tapo import probe_l530e


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="cardputer-probe", description="Secret-safe live LAN probe for Tapo L530E and Dyson TP09")
    result.add_argument("target", choices=("all", "tapo", "dyson", "dyson-local"), nargs="?", default="all")
    result.add_argument("--write-test", action="store_true", help="perform reversible state changes required by the firmware gate")
    result.add_argument("--tapo-host", help="L530E IPv4 address if UDP discovery is blocked")
    result.add_argument("--dyson-host", help="TP09 IPv4 address if mDNS discovery is blocked")
    result.add_argument("--country", default="TW", help="MyDyson account country (default: TW)")
    result.add_argument("--culture", default="zh-TW", help="MyDyson API culture (default: zh-TW)")
    result.add_argument("--timeout", type=int, default=10)
    result.add_argument("--report", type=Path, default=Path("probe-report.json"))
    result.add_argument("--save-dyson-credential", action="store_true", help="save firmware import material under ignored .secrets/")
    return result


def _error_record(exc: Exception) -> dict[str, Any]:
    # Third-party exception strings are not trusted because some include request
    # details. Keep the useful classification without risking a credential leak.
    if isinstance(exc, ProbeStageError):
        return exc.to_report()
    return {"error_type": type(exc).__name__, "error": "Live probe failed; see console classification and retry guidance"}


def _error_summary(exc: Exception) -> str:
    if isinstance(exc, ProbeStageError):
        return f"{exc.stage}/{exc.code}"
    return type(exc).__name__


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    logging.basicConfig(level=logging.WARNING, format="%(levelname)s: %(message)s")
    report = new_report()
    merge_existing_devices(args.report, report)
    forbidden: list[str] = []
    exit_code = 0

    if args.target in {"all", "tapo"}:
        print("\nTapo L530E credentials (hidden input; never written to the report)")
        tapo_email = getpass("Tapo account email: ").strip()
        tapo_password = getpass("Tapo account password: ")
        forbidden.extend([tapo_email, tapo_password])
        try:
            device_report = asyncio.run(
                probe_l530e(tapo_email, tapo_password, args.tapo_host, args.timeout, args.write_test)
            )
            device_report["tested_at"] = report["generated_at"]
            report["devices"]["tapo_l530e"] = device_report
            print("Tapo probe completed.")
        except Exception as exc:
            error_report = _error_record(exc)
            error_report["tested_at"] = report["generated_at"]
            report["devices"]["tapo_l530e"] = error_report
            print(f"Tapo probe failed: {_error_summary(exc)}. See the redacted report.", file=sys.stderr)
            exit_code = 1

    if args.target in {"all", "dyson"}:
        print("\nMyDyson credentials (cloud is used only now to obtain the local MQTT credential)")
        dyson_email = getpass("MyDyson account email: ").strip()
        dyson_password = getpass("MyDyson account password: ")
        forbidden.extend([dyson_email, dyson_password])
        try:
            dyson_report, dyson_secrets = probe_tp09(
                email=dyson_email, password=dyson_password, country=args.country, culture=args.culture,
                host=args.dyson_host, timeout=args.timeout, write_test=args.write_test,
                save_credential=args.save_dyson_credential, secrets_dir=Path(".secrets"),
            )
            forbidden.extend(dyson_secrets)
            dyson_report["tested_at"] = report["generated_at"]
            report["devices"]["dyson_tp09"] = dyson_report
            print("Dyson probe completed.")
        except Exception as exc:
            error_report = _error_record(exc)
            error_report["tested_at"] = report["generated_at"]
            report["devices"]["dyson_tp09"] = error_report
            print(f"Dyson probe failed: {_error_summary(exc)}. See the redacted report.", file=sys.stderr)
            exit_code = 1

    if args.target == "dyson-local":
        print("\nDyson saved local credential (MyDyson cloud will not be contacted)")
        try:
            dyson_report, dyson_secrets = probe_saved_tp09(
                Path(".secrets") / "dyson-local.json", timeout=args.timeout, write_test=args.write_test
            )
            forbidden.extend(dyson_secrets)
            previous = report["devices"].get("dyson_tp09", {})
            for key in ("firmware_version",):
                if key in previous:
                    dyson_report.setdefault(key, previous[key])
            dyson_report["tested_at"] = report["generated_at"]
            report["devices"]["dyson_tp09"] = dyson_report
            print("Dyson local probe completed.")
        except Exception as exc:
            error_report = _error_record(exc)
            error_report["tested_at"] = report["generated_at"]
            report["devices"]["dyson_tp09"] = error_report
            print(f"Dyson local probe failed: {_error_summary(exc)}. See the redacted report.", file=sys.stderr)
            exit_code = 1

    evaluate_gate(report)
    write_report(args.report, report, forbidden)
    print(f"\nRedacted report: {args.report.resolve()}")
    print("Firmware gate:", "PASS" if report["gate"]["passed"] else "NOT PASSED")
    return exit_code if exit_code else (0 if report["gate"]["passed"] or not args.write_test else 2)


if __name__ == "__main__":
    raise SystemExit(main())
