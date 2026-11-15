#!/usr/bin/env python3
"""Generate a controller's round display setup code as an NVS CSV.

The code is 8 random bytes shown as 16 uppercase hex digits (pairing v1). The
firmware reads it from NVS namespace `display_link`, key `setup_code`, and uses
it as the Security1 proof of possession in setup mode. The CSV is written
outside the repository with owner-only permissions; label the controller with
the printed code, then build and flash the NVS image as printed.
"""

from __future__ import annotations

import argparse
import os
import re
import secrets
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUT = Path.home() / ".mazda-display-provisioning"
CSV_NAME = "display_setup_nvs.csv"
NVS_OFFSET = "0x9000"  # partitions.csv
NVS_SIZE = "0x10000"


def generate_code() -> str:
    return secrets.token_bytes(8).hex().upper()


def normalize_code(text: str) -> str:
    code = re.sub(r"[ -]", "", text).upper()
    if not re.fullmatch(r"[0-9A-F]{16}", code):
        raise ValueError("a setup code is exactly 16 hex digits (spaces and hyphens allowed)")
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"output directory outside the repository (default {DEFAULT_OUT})")
    parser.add_argument("--code", help="reuse an existing label's code instead of generating one")
    args = parser.parse_args(argv)

    out = args.out.expanduser().resolve()
    if out == REPO or REPO in out.parents:
        print(f"error: {out} is not outside the repository; setup codes are secrets")
        return 2
    try:
        code = normalize_code(args.code) if args.code else generate_code()
    except ValueError as error:
        print(f"error: {error}")
        return 2

    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    path = out / CSV_NAME
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w", newline="") as handle:
        handle.write("key,type,encoding,value\n"
                     "display_link,namespace,,\n"
                     f"setup_code,data,string,{code}\n")

    print(f"Setup code (label the controller): {code}")
    print(f"NVS CSV: {path}")
    print("Build and flash it (this REPLACES the whole NVS partition: saved lighting")
    print("config, BLE bonds and any display pairing):")
    print(f"  python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py "
          f"generate {path} {out / 'display_setup_nvs.bin'} {NVS_SIZE}")
    print(f"  esptool.py --chip esp32 -p <PORT> write_flash {NVS_OFFSET} {out / 'display_setup_nvs.bin'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
