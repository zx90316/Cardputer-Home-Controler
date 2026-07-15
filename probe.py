"""Repository-local launcher that does not require an editable package install."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from cardputer_probe.cli import main  # noqa: E402

raise SystemExit(main())
