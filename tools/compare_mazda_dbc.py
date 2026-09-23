#!/usr/bin/env python3
"""Compare the supported reviewed Mazda DBC subset with compiled metadata.

The C++ constexpr table is the executable metadata authority. This host-only
tool parses the reviewed source artifact and consumes output from the compiled
metadata dumper; it does not parse DBC at runtime, generate a schema, or alter
decoder definitions.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Iterable, Mapping, Sequence


_MESSAGE_RE = re.compile(r"^BO_\s+(?P<identifier>\d+)\s+(?P<name>[^\s:]+)\s*:")
_SIGNAL_RE = re.compile(
    r"^\s+SG_\s+(?P<name>[^\s:]+)\s*:\s*"
    r"(?P<start>\d+)\|(?P<length>\d+)@(?P<byte_order>[01])(?P<sign>[+-])\s*"
    r"\((?P<scale>[^,]+),(?P<offset>[^)]+)\)\s*"
    r"\[(?P<minimum>[^|]+)\|(?P<maximum>[^\]]+)\]\s*"
    r'"(?P<unit>[^"]*)"\s+\S+'
)

_METADATA_FIELDS = (
    "message",
    "dbc_signal",
    "channel",
    "name",
    "identifier",
    "start_bit",
    "dbc_start_bit",
    "bit_length",
    "scale",
    "offset",
    "physical_min",
    "physical_max",
    "byte_order",
    "unit",
    "confidence",
    "provenance",
)

# This is the narrow, reviewed S1-E source-name boundary. In particular,
# FrontOtherDoor -> front_left_door_open_rhd is an interpretation exception;
# no other source field is silently renamed or dropped from this subset.
_EXPECTED_CONFIDENCE = {
    ("ENGINE_DATA", "EngineRPM"): "Confirmed",
    ("TRANSMISSION", "Selector"): "Confirmed",
    ("TRANSMISSION", "ActualGear"): "Observed",
    ("DOORS", "Liftgate_Open_Reference"): "Reference",
    ("DOORS", "RearRightDoor_Open_Reference"): "Reference",
    ("DOORS", "RearLeftDoor_Open_Reference"): "Reference",
    ("DOORS", "FrontOtherDoor_Open_Reference"): "Reference",
    ("DOORS", "FrontRightDoor_Open_RHD"): "Confirmed",
    ("DOORS", "DoorsUnlocked_Reference"): "Reference",
    ("BLINK_INFO", "LeftIndicatorLamp_Reference"): "Reference",
    ("BLINK_INFO", "RightIndicatorLamp_Reference"): "Reference",
    ("BLINK_INFO", "WiperLow_Reference"): "Observed",
    ("TURN_SWITCH", "HazardSwitch_Reference"): "Reference",
    ("TURN_SWITCH", "RightIndicatorSwitch_Reference"): "Reference",
    ("TURN_SWITCH", "LeftIndicatorSwitch_Reference"): "Reference",
    ("TURN_SWITCH", "FrontWiper"): "Observed",
    ("BRAKE_PEDAL", "BrakePressed"): "Confirmed",
}

# Stable application-facing channel names are an independent contract from
# the reviewed DBC source names. Keep this mapping frozen here so a change in
# kSupportedSignalDefinitions cannot silently change the channel used by the
# evidence inventory. FrontOtherDoor's mapping is the sole documented
# interpretation exception.
_EXPECTED_CHANNEL = {
    ("ENGINE_DATA", "EngineRPM"): "engine_rpm",
    ("TRANSMISSION", "Selector"): "selector_position",
    ("TRANSMISSION", "ActualGear"): "actual_gear",
    ("DOORS", "Liftgate_Open_Reference"): "liftgate_open",
    ("DOORS", "RearRightDoor_Open_Reference"): "rear_right_door_open",
    ("DOORS", "RearLeftDoor_Open_Reference"): "rear_left_door_open",
    ("DOORS", "FrontOtherDoor_Open_Reference"): "front_left_door_open_rhd",
    ("DOORS", "FrontRightDoor_Open_RHD"): "front_right_door_open_rhd",
    ("DOORS", "DoorsUnlocked_Reference"): "doors_unlocked",
    ("BLINK_INFO", "LeftIndicatorLamp_Reference"): "left_indicator_lamp",
    ("BLINK_INFO", "RightIndicatorLamp_Reference"): "right_indicator_lamp",
    ("BLINK_INFO", "WiperLow_Reference"): "wiper_low",
    ("TURN_SWITCH", "HazardSwitch_Reference"): "hazard_request",
    ("TURN_SWITCH", "RightIndicatorSwitch_Reference"): "right_turn_request",
    ("TURN_SWITCH", "LeftIndicatorSwitch_Reference"): "left_turn_request",
    ("TURN_SWITCH", "FrontWiper"): "front_wiper",
    ("BRAKE_PEDAL", "BrakePressed"): "brake_pressed",
}

_EXPECTED_UNIT = {
    ("ENGINE_DATA", "EngineRPM"): "RevolutionsPerMinute",
    ("TRANSMISSION", "Selector"): "None",
    ("TRANSMISSION", "ActualGear"): "None",
    ("TURN_SWITCH", "FrontWiper"): "None",
}
_EXPECTED_UNIT.update(
    {
        key: "Boolean"
        for key in (
            ("DOORS", "Liftgate_Open_Reference"),
            ("DOORS", "RearRightDoor_Open_Reference"),
            ("DOORS", "RearLeftDoor_Open_Reference"),
            ("DOORS", "FrontOtherDoor_Open_Reference"),
            ("DOORS", "FrontRightDoor_Open_RHD"),
            ("DOORS", "DoorsUnlocked_Reference"),
            ("BLINK_INFO", "LeftIndicatorLamp_Reference"),
            ("BLINK_INFO", "RightIndicatorLamp_Reference"),
            ("BLINK_INFO", "WiperLow_Reference"),
            ("TURN_SWITCH", "HazardSwitch_Reference"),
            ("TURN_SWITCH", "RightIndicatorSwitch_Reference"),
            ("TURN_SWITCH", "LeftIndicatorSwitch_Reference"),
            ("BRAKE_PEDAL", "BrakePressed"),
        )
    }
)


@dataclass(frozen=True)
class DbcSignal:
    message: str
    identifier: int
    name: str
    start: int
    length: int
    byte_order: str
    sign: str
    scale: Decimal
    offset: Decimal
    minimum: Decimal
    maximum: Decimal
    unit: str


@dataclass(frozen=True)
class MetadataSignal:
    message: str
    dbc_signal: str
    channel: str
    name: str
    identifier: int
    start_bit: int
    dbc_start_bit: int
    bit_length: int
    scale: Decimal
    offset: Decimal
    physical_min: Decimal
    physical_max: Decimal
    byte_order: str
    unit: str
    confidence: str
    provenance: str


def _decimal(text: str, label: str) -> Decimal:
    try:
        return Decimal(text.strip())
    except InvalidOperation as error:
        raise ValueError(f"invalid {label} value {text!r}") from error


def parse_dbc(text: str) -> dict[tuple[str, str], DbcSignal]:
    """Parse only the DBC syntax required by the supported signal subset."""

    signals: dict[tuple[str, str], DbcSignal] = {}
    current: tuple[int, str] | None = None
    for line_number, line in enumerate(text.splitlines(), start=1):
        message_match = _MESSAGE_RE.match(line)
        if message_match:
            current = (int(message_match.group("identifier")), message_match.group("name"))
            continue
        signal_match = _SIGNAL_RE.match(line)
        if not signal_match:
            continue
        if current is None:
            raise ValueError(f"signal before message at DBC line {line_number}")
        groups = signal_match.groupdict()
        signal = DbcSignal(
            message=current[1],
            identifier=current[0],
            name=groups["name"],
            start=int(groups["start"]),
            length=int(groups["length"]),
            byte_order="Intel" if groups["byte_order"] == "1" else "Motorola",
            sign=groups["sign"],
            scale=_decimal(groups["scale"], "scale"),
            offset=_decimal(groups["offset"], "offset"),
            minimum=_decimal(groups["minimum"], "minimum"),
            maximum=_decimal(groups["maximum"], "maximum"),
            unit=groups["unit"],
        )
        key = (signal.message, signal.name)
        if key in signals:
            raise ValueError(f"duplicate DBC signal {signal.message}.{signal.name}")
        signals[key] = signal
    return signals


def parse_metadata(text: str) -> dict[tuple[str, str], MetadataSignal]:
    lines = [line for line in text.splitlines() if line.strip()]
    if not lines or lines[0] != "mazda-metadata-v1":
        raise ValueError("metadata dumper version header is missing")
    if len(lines) < 2 or tuple(lines[1].split("\t")) != _METADATA_FIELDS:
        raise ValueError("metadata dumper header does not match mazda-metadata-v1")

    metadata: dict[tuple[str, str], MetadataSignal] = {}
    expected_count = len(_METADATA_FIELDS)
    for line_number, line in enumerate(lines[2:], start=3):
        fields = line.split("\t")
        if len(fields) != expected_count:
            raise ValueError(f"metadata line {line_number} has {len(fields)} fields, expected {expected_count}")
        (
            message,
            dbc_signal,
            channel,
            name,
            identifier,
            start_bit,
            dbc_start_bit,
            bit_length,
            scale,
            offset,
            physical_min,
            physical_max,
            byte_order,
            unit,
            confidence,
            provenance,
        ) = fields
        result = MetadataSignal(
            message=message,
            dbc_signal=dbc_signal,
            channel=channel,
            name=name,
            identifier=int(identifier),
            start_bit=int(start_bit),
            dbc_start_bit=int(dbc_start_bit),
            bit_length=int(bit_length),
            scale=_decimal(scale, "metadata scale"),
            offset=_decimal(offset, "metadata offset"),
            physical_min=_decimal(physical_min, "metadata physical minimum"),
            physical_max=_decimal(physical_max, "metadata physical maximum"),
            byte_order=byte_order,
            unit=unit,
            confidence=confidence,
            provenance=provenance,
        )
        key = (message, dbc_signal)
        if key in metadata:
            raise ValueError(f"duplicate metadata signal {message}.{dbc_signal}")
        metadata[key] = result
    return metadata


def decoder_start_bit(signal: DbcSignal) -> int:
    """Return the lowest linear payload bit coordinate covered by a DBC signal.

    Intel DBC signals already identify their least-significant payload bit with
    ``start``. Motorola signals identify their most-significant bit in the DBC
    sawtooth coordinate system: walking toward the least-significant bit moves
    down within a byte and wraps from bit 0 to bit 7 of the next byte. The
    decoder metadata uses ordinary byte/LSB coordinates, so the lowest covered
    coordinate is the decoder-facing start bit.
    """

    if signal.length <= 0:
        raise ValueError(f"{signal.message}.{signal.name} has invalid bit length {signal.length}")
    if signal.byte_order == "Intel":
        return signal.start
    if signal.byte_order != "Motorola":
        raise ValueError(
            f"{signal.message}.{signal.name} has invalid byte order {signal.byte_order!r}"
        )

    coordinate = signal.start
    lowest = coordinate
    for _ in range(1, signal.length):
        coordinate = coordinate - 1 if coordinate % 8 else coordinate + 15
        lowest = min(lowest, coordinate)
    return lowest


def compare(dbc: Mapping[tuple[str, str], DbcSignal],
            metadata: Mapping[tuple[str, str], MetadataSignal]) -> list[str]:
    failures: list[str] = []
    expected_keys = set(_EXPECTED_CONFIDENCE)
    actual_keys = set(metadata)
    missing = expected_keys - actual_keys
    unexpected = actual_keys - expected_keys
    if missing:
        failures.append("metadata is missing supported signals: " + ", ".join(
            f"{message}.{name}" for message, name in sorted(missing)
        ))
    if unexpected:
        failures.append("metadata contains unsupported signals: " + ", ".join(
            f"{message}.{name}" for message, name in sorted(unexpected)
        ))
    source_missing = expected_keys - set(dbc)
    if source_missing:
        failures.append("DBC is missing supported signals: " + ", ".join(
            f"{message}.{name}" for message, name in sorted(source_missing)
        ))

    for key in sorted(expected_keys & actual_keys & set(dbc)):
        source = dbc[key]
        definition = metadata[key]
        label = f"{source.message}.{source.name}"
        checks: Iterable[tuple[str, object, object]] = (
            ("channel", _EXPECTED_CHANNEL[key], definition.channel),
            ("identifier", source.identifier, definition.identifier),
            ("decoder start bit", decoder_start_bit(source), definition.start_bit),
            ("start bit", source.start, definition.dbc_start_bit),
            ("bit length", source.length, definition.bit_length),
            ("byte order", source.byte_order, definition.byte_order),
            ("scale", source.scale, definition.scale),
            ("offset", source.offset, definition.offset),
            ("physical minimum", source.minimum, definition.physical_min),
            ("physical maximum", source.maximum, definition.physical_max),
            ("unit", _EXPECTED_UNIT[key], definition.unit),
        )
        expected_dbc_unit = "rpm" if key == ("ENGINE_DATA", "EngineRPM") else ""
        checks = (*checks, ("DBC unit", expected_dbc_unit, source.unit))
        if source.sign != "+":
            failures.append(f"{label} is signed in the DBC; supported metadata requires unsigned '+'")
        for field, expected, actual in checks:
            if expected != actual:
                failures.append(f"{label} {field} mismatch: DBC {expected} != metadata {actual}")

        expected_confidence = _EXPECTED_CONFIDENCE[key]
        if definition.confidence != expected_confidence:
            failures.append(
                f"{label} confidence mismatch: expected S1-E {expected_confidence}, "
                f"metadata reports {definition.confidence}"
            )
        if not definition.provenance:
            failures.append(f"{label} has no provenance reference")
        elif "signal-evidence.md" not in definition.provenance or "mazda_custom.dbc" not in definition.provenance:
            failures.append(f"{label} provenance must cite signal-evidence.md and mazda_custom.dbc")
        if expected_confidence == "Confirmed" and "#L" not in definition.provenance:
            failures.append(f"{label} Confirmed assignment must cite a specific DBC evidence line")

    return failures


def _run_dumper(root: Path, compiler: str, core_root: Path) -> str:
    source = root / "tools/dump_mazda_metadata.cpp"
    if not source.is_file():
        raise FileNotFoundError(f"metadata dumper is missing: {source}")
    with tempfile.TemporaryDirectory(prefix="mazda-metadata-") as temporary:
        executable = Path(temporary) / "dump_mazda_metadata"
        compile_command = [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            f"-I{root / 'lib/mazda/include'}",
            f"-I{core_root / 'components/vehicle_core/include'}",
            str(source),
            "-o",
            str(executable),
        ]
        compiled = subprocess.run(compile_command, check=False, capture_output=True, text=True)
        if compiled.returncode != 0:
            detail = (compiled.stdout + compiled.stderr).strip()
            raise RuntimeError(f"metadata dumper compilation failed: {detail}")
        dumped = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
        if dumped.returncode != 0:
            detail = (dumped.stdout + dumped.stderr).strip()
            raise RuntimeError(f"metadata dumper failed: {detail}")
        return dumped.stdout


def _run_existing_dumper(executable: Path) -> str:
    dumped = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
    if dumped.returncode != 0:
        detail = (dumped.stdout + dumped.stderr).strip()
        raise RuntimeError(f"metadata dumper failed: {detail}")
    return dumped.stdout


def _resolve_compiler(requested: str | None) -> str:
    if requested:
        return requested
    return os.environ.get("CXX", "c++")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core-root", type=Path)
    parser.add_argument("--dbc", type=Path)
    parser.add_argument("--metadata-file", type=Path,
                        help="use an existing dumper output instead of compiling the dumper")
    parser.add_argument("--metadata-dumper", type=Path,
                        help="run an already-built metadata dumper executable")
    parser.add_argument("--compiler", help="host C++ compiler used for the metadata dumper")
    parser.add_argument("--dump-metadata", action="store_true",
                        help="print compiled metadata and skip DBC comparison")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    core_root = (args.core_root or root / "third_party/esp32-vehicle-can-core").resolve()
    dbc_path = (args.dbc or root / "docs/protocol/mazda_custom.dbc").resolve()
    try:
        if args.metadata_file:
            metadata_text = args.metadata_file.read_text(encoding="utf-8")
        elif args.metadata_dumper:
            metadata_text = _run_existing_dumper(args.metadata_dumper.resolve())
        else:
            metadata_text = _run_dumper(root, _resolve_compiler(args.compiler), core_root)
        if args.dump_metadata:
            print(metadata_text, end="")
            return 0
        dbc = parse_dbc(dbc_path.read_text(encoding="utf-8"))
        metadata = parse_metadata(metadata_text)
        failures = compare(dbc, metadata)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    print(f"DBC metadata comparison passed ({len(_EXPECTED_CONFIDENCE)} supported signals)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
