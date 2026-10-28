#!/usr/bin/env python3
"""Verify that every symbol pinned by an ldgen fragment landed in IRAM.

ESP-IDF's ldgen silently ignores a symbol entry that matches nothing, so a
renamed, inlined, or cloned hot function would quietly fall back to flash.
This checker reads the symbol entries of a linker fragment, resolves the
`.iram0.text` bounds with the toolchain `objdump -h`, lists the ELF symbols
with the toolchain `nm`, and fails unless every listed symbol is defined
inside that section. Optional `# check-local-code:` comments also check code
whose demangled name includes a stable parent function identity. The firmware
build runs it as a POST_BUILD step.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence


IRAM_SECTION = ".iram0.text"

# `object:symbol (scheme)` entries; scheme-only entries such as `* (noflash)`
# or `object (noflash)` place whole inputs and name no symbol.
_SYMBOL_ENTRY = re.compile(r"^\s*[^\s:#]+:(?P<symbol>[^\s(]+)\s*\((?P<scheme>[^)]+)\)\s*$")
_CODE_SCHEMES = frozenset({"noflash", "noflash_text", "iram"})
_LOCAL_CODE_PARENT = re.compile(r"^\s*#\s*check-local-code:\s*(.+?)\s*$")


@dataclass(frozen=True)
class Section:
    name: str
    start: int
    size: int

    def contains(self, address: int) -> bool:
        return self.start <= address < self.start + self.size


def parse_fragment_symbols(text: str) -> List[str]:
    """Return the symbols that the fragment maps with an IRAM code scheme."""
    symbols: List[str] = []
    for line in text.splitlines():
        match = _SYMBOL_ENTRY.match(line)
        if match and match.group("scheme").strip() in _CODE_SCHEMES:
            symbols.append(match.group("symbol"))
    return symbols


def parse_section(objdump_headers: str, name: str) -> Section:
    """Find a section's VMA and size in `objdump -h` output."""
    for line in objdump_headers.splitlines():
        fields = line.split()
        # Idx Name Size VMA LMA File-off Algn
        if len(fields) >= 4 and fields[0].isdigit() and fields[1] == name:
            return Section(name, int(fields[3], 16), int(fields[2], 16))
    raise ValueError(f"section {name} not found in ELF headers")


def parse_nm(nm_output: str) -> Dict[str, List[int]]:
    """Map each defined symbol name to its addresses from `nm` output."""
    symbols: Dict[str, List[int]] = {}
    for line in nm_output.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue  # undefined symbols have no address
        address, _kind, name = fields
        try:
            symbols.setdefault(name, []).append(int(address, 16))
        except ValueError:
            continue
    return symbols


def parse_local_code_parents(text: str) -> List[str]:
    """Read demangled parent identities from checker-only fragment comments."""
    return [match.group(1) for line in text.splitlines()
            if (match := _LOCAL_CODE_PARENT.match(line))]


def local_code_symbols(nm_output: str, demangled_output: str,
                       parents: Sequence[str]) -> List[str]:
    """Find code referring to a parent's local types without mangling assumptions.

    Both nm calls use --no-sort, preserving ELF symbol-table order. Demangling
    resolves context-dependent ABI substitutions in std::apply/invoke helpers.
    Check paired addresses/types before using the demangled identity.
    """
    def records(text: str) -> list[tuple[int, str, str]]:
        result = []
        for line in text.splitlines():
            fields = line.split(maxsplit=2)
            if len(fields) == 3:
                try:
                    result.append((int(fields[0], 16), fields[1], fields[2]))
                except ValueError:
                    pass
        return result

    raw = records(nm_output)
    demangled = records(demangled_output)
    if len(raw) != len(demangled) or any(
            left[:2] != right[:2] for left, right in zip(raw, demangled)):
        raise ValueError("raw and demangled nm symbol tables do not align")
    result = set()
    for parent in parents:
        matches = [left[2] for left, right in zip(raw, demangled)
                   if left[1] in {"T", "t", "W", "w"} and parent in right[2]]
        if not matches:
            raise ValueError(f"local code parent not found in ELF: {parent}")
        result.update(matches)
    return sorted(result)


def check(symbols: Sequence[str], section: Section, defined: Dict[str, List[int]]) -> List[str]:
    """Return one failure message per symbol not placed in the section."""
    failures: List[str] = []
    for symbol in symbols:
        addresses = defined.get(symbol)
        if not addresses:
            failures.append(f"missing from ELF: {symbol}")
        elif not any(section.contains(address) for address in addresses):
            placed = ", ".join(f"0x{address:08x}" for address in addresses)
            failures.append(
                f"not in {section.name} [0x{section.start:08x}, "
                f"0x{section.start + section.size:08x}) at {placed}: {symbol}"
            )
    return failures


def _run(command: Sequence[str]) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fragment", type=Path, required=True, help="ldgen .lf fragment")
    parser.add_argument("--elf", type=Path, required=True, help="linked application ELF")
    parser.add_argument("--nm", default="xtensa-esp32-elf-nm", help="toolchain nm executable")
    parser.add_argument(
        "--objdump", default="xtensa-esp32-elf-objdump", help="toolchain objdump executable"
    )
    args = parser.parse_args(argv)

    fragment = args.fragment.read_text(encoding="utf-8")
    symbols = parse_fragment_symbols(fragment)
    parents = parse_local_code_parents(fragment)
    if not symbols:
        print(f"IRAM placement check failed: no symbol entries in {args.fragment}", file=sys.stderr)
        return 1
    try:
        section = parse_section(_run([args.objdump, "-h", str(args.elf)]), IRAM_SECTION)
        nm_output = _run([args.nm, "--defined-only", "--no-sort", str(args.elf)])
        defined = parse_nm(nm_output)
        if parents:
            demangled = _run([args.nm, "--defined-only", "--no-sort", "--demangle", str(args.elf)])
            symbols = list(dict.fromkeys(symbols + local_code_symbols(nm_output, demangled, parents)))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"IRAM placement check failed: {error}", file=sys.stderr)
        return 1

    failures = check(symbols, section, defined)
    if failures:
        print(
            f"IRAM placement check failed: {len(failures)} of {len(symbols)} symbols from "
            f"{args.fragment.name} are not in {IRAM_SECTION}. ldgen ignores entries that match "
            "no symbol; regenerate the mangled names (docs/development/flash-cache-layout.md).",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"IRAM placement check passed: {len(symbols)} symbols from {args.fragment.name} in {IRAM_SECTION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
