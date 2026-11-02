#!/usr/bin/env python3
"""Reconfigure the host replay against two imported profiles and an empty one."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
TARGET = "replay_selected_profile_controller_tests"


def write_profile(directory: Path, profile_id: str, identifier_offset: int) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    sources = []
    for name in ("demo-a.dbc", "demo-b.dbc"):
        content = (FIXTURES / name).read_text(encoding="utf-8")
        if identifier_offset:
            content = content.replace("BO_ 256 Engine:", "BO_ 512 Engine:")
            content = content.replace("BO_ 256 EngineExtra:", "BO_ 512 EngineExtra:")
            brake_id = 145 + identifier_offset
        else:
            brake_id = 145
        content += (
            f"\nBO_ {brake_id} BrakeInput: 8 ECU\n"
            ' SG_ pressed : 0|1@1+ (1,0) [0|1] "" Controller\n'
        )
        source_path = directory / name
        source_path.write_text(content, encoding="utf-8")
        sources.append(
            {
                "path": name,
                "sha256": hashlib.sha256(source_path.read_bytes()).hexdigest(),
                "bus": "body",
                "license": "Apache-2.0; synthetic fixture",
                "encoding": "utf-8",
            }
        )

    mapping = {
        "version": 1,
        "profile_id": profile_id,
        "selected_bus": "body",
        "sources": sources,
        "bindings": [
            {
                "target": "vehicle.engine_rpm",
                "message": "Engine",
                "field": "rpm",
                "unit": "rpm",
                "timeout_ms": 100,
            },
            {
                "target": "vehicle.brake_pressed",
                "message": "BrakeInput",
                "field": "pressed",
                "unit": "",
                "timeout_ms": 100,
            },
        ],
    }
    mapping_path = directory / "mapping.json"
    mapping_path.write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")
    return mapping_path


def configure_case(
    args: argparse.Namespace,
    mapping: Path,
    expect_empty: bool,
    expected_engine_id: int | None = None,
    expected_brake_id: int | None = None,
) -> None:
    command = [
        args.cmake,
        "-S",
        str(ROOT),
        "-B",
        str(args.build_dir),
        "-G",
        args.generator,
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DBUILD_TESTING=ON",
        "-DMAZDA_BUILD_HOST_TESTS=ON",
        f"-DVEHICLE_PROFILE={mapping}",
        f"-DREPLAY_SELECTED_PROFILE_EXPECT_EMPTY={1 if expect_empty else 0}",
    ]
    if expected_engine_id is not None:
        command.append(f"-DREPLAY_SELECTED_PROFILE_EXPECT_ENGINE_ID={expected_engine_id}")
    if expected_brake_id is not None:
        command.append(f"-DREPLAY_SELECTED_PROFILE_EXPECT_BRAKE_ID={expected_brake_id}")
    if args.core_source_dir is not None:
        command.append(f"-DVEHICLE_CAN_CORE_SOURCE_DIR={args.core_source_dir}")
    subprocess.run(command, check=True)

    build = [args.cmake, "--build", str(args.build_dir), "--target", TARGET, "--parallel"]
    if args.jobs:
        build.append(str(args.jobs))
    subprocess.run(build, check=True)

    executable = args.build_dir / "tests/host" / TARGET
    if executable.with_suffix(".exe").exists():
        executable = executable.with_suffix(".exe")
    subprocess.run([str(executable)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--core-source-dir", type=Path)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()

    profile_a = write_profile(args.build_dir / "replay-fixture-a", "synthetic.replay.a", 0)
    profile_b = write_profile(args.build_dir / "replay-fixture-b", "synthetic.replay.b", 1)
    empty = FIXTURES / "mapping-empty.json"

    for label, mapping, expect_empty, engine_id, brake_id in (
        ("two-source profile A", profile_a, False, 256, 145),
        ("alternate CAN layout profile B", profile_b, False, 512, 146),
        ("empty profile", empty, True, None, None),
    ):
        print(f"\n== {label} ==", flush=True)
        configure_case(args, mapping, expect_empty, engine_id, brake_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
