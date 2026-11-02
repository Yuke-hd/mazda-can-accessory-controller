#!/usr/bin/env python3
"""Embed generated factory JSON in a collision-safe C++ raw string header."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def render_header(payload: bytes) -> bytes:
    document = payload.decode("ascii")
    delimiter = ""
    for nonce in range(256):
        candidate = f"F{hashlib.sha256(payload + bytes([nonce])).hexdigest()[:15]}"
        if f"){candidate}\"" not in document:
            delimiter = candidate
            break
    if not delimiter:
        raise ValueError("could not find a C++ raw-string delimiter for factory JSON")

    source = f'''#pragma once

#include <string_view>

namespace replay::generated {{

inline constexpr char kFactoryConfigJsonData[] = R"{delimiter}({document}){delimiter}";
inline constexpr std::string_view kFactoryConfigJson{{kFactoryConfigJsonData,
                                                      sizeof(kFactoryConfigJsonData) - 1U}};

}} // namespace replay::generated
'''
    return source.encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    payload = args.input.read_bytes()
    output = render_header(payload)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != output:
        args.output.write_bytes(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
