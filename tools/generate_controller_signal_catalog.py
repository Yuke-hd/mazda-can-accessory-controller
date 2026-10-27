#!/usr/bin/env python3
"""Regenerate or check the checked-in compiler catalog from the host exporter."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


CATALOG_PATH = Path(__file__).with_name("controller_signal_catalog.json")
CATALOG_SOURCE = "components/mazda_telemetry/private_include/mazda/signal_catalog.hpp"


def render_manifest(exported: str) -> bytes:
    signals = json.loads(exported)
    if not isinstance(signals, dict):
        raise ValueError("catalog exporter must emit a signal object")
    return (json.dumps({"version": 1, "source": CATALOG_SOURCE, "signals": signals},
                       indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def generate(exporter: Path, output: Path, *, check: bool) -> bool:
    result = subprocess.run([str(exporter.resolve())], check=True, capture_output=True,
                            text=True, encoding="utf-8")
    expected = render_manifest(result.stdout)
    if check:
        if not output.is_file() or output.read_bytes() != expected:
            print(f"{output}: stale controller signal catalog; regenerate with "
                  "the regenerate_controller_signal_catalog CMake target", file=sys.stderr)
            return False
    else:
        output.write_bytes(expected)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True,
                        help="built host export_controller_signal_catalog executable")
    parser.add_argument("--output", type=Path, default=CATALOG_PATH)
    parser.add_argument("--check", action="store_true", help="fail on drift without writing")
    args = parser.parse_args()
    try:
        return 0 if generate(args.exporter, args.output, check=args.check) else 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"controller signal catalog: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
