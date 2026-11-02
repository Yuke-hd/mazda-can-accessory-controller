#!/usr/bin/env python3
"""Reconfigure replay and config integration against imported and empty profiles."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
TARGET = "replay_selected_profile_controller_tests"
CONFIG_TARGET = "selected_profile_config_tests"
FACTORY_JSON_TARGET = "controller_config_factory_json"
FACTORY_HEADER_TARGET = "replay_factory_config_header"


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
        f"-DCONTROLLER_CONFIG_DEFAULT_YAML={ROOT / 'config/default.yaml'}",
        f"-DREPLAY_SELECTED_PROFILE_EXPECT_ENGINE_ID={expected_engine_id or ''}",
        f"-DREPLAY_SELECTED_PROFILE_EXPECT_BRAKE_ID={expected_brake_id or ''}",
    ]
    if args.core_source_dir is not None:
        command.append(f"-DVEHICLE_CAN_CORE_SOURCE_DIR={args.core_source_dir}")
    subprocess.run(command, check=True)

    replay_executable = None
    for target in (TARGET, CONFIG_TARGET):
        build = [args.cmake, "--build", str(args.build_dir), "--target", target, "--parallel"]
        if args.jobs:
            build.append(str(args.jobs))
        subprocess.run(build, check=True)

        executable = args.build_dir / "tests/host" / target
        if executable.with_suffix(".exe").exists():
            executable = executable.with_suffix(".exe")
        subprocess.run([str(executable)], check=True)
        if target == TARGET:
            replay_executable = executable

    if expected_engine_id == 256 and expected_brake_id == 145:
        verify_embedded_factory_config(args, command, replay_executable)


def verify_embedded_factory_config(
    args: argparse.Namespace, configure_command: list[str], executable: Path
) -> None:
    """Prove an old copied replay binary is independent of factory artifacts."""
    relocated = args.build_dir / "relocated-old-replay"
    relocated.mkdir(parents=True, exist_ok=True)
    copied_executable = relocated / executable.name
    shutil.copy2(executable, copied_executable)

    factory_json = args.build_dir / "factory_default_config.json"
    original_json = factory_json.read_bytes()
    changed_config = json.loads(original_json)
    changed_config["outputs"][0]["color"]["blue"] = 0
    changed_json = json.dumps(changed_config, sort_keys=True, separators=(",", ":")) + "\n"
    mutated_yaml = relocated / "mutated-factory.yaml"
    mutated_yaml.write_text(changed_json, encoding="utf-8")

    def run_copy() -> None:
        subprocess.run([str(copied_executable)], check=True, cwd=relocated)

    try:
        factory_json.write_text(changed_json, encoding="utf-8")
        run_copy()

        reconfigure = [
            argument
            for argument in configure_command
            if not argument.startswith("-DCONTROLLER_CONFIG_DEFAULT_YAML=")
        ]
        reconfigure.append(f"-DCONTROLLER_CONFIG_DEFAULT_YAML={mutated_yaml}")
        subprocess.run(reconfigure, check=True)
        subprocess.run(
            [args.cmake, "--build", str(args.build_dir), "--target", FACTORY_JSON_TARGET],
            check=True,
        )
        subprocess.run(
            [args.cmake, "--build", str(args.build_dir), "--target", FACTORY_HEADER_TARGET],
            check=True,
        )
        run_copy()

        factory_json.unlink()
        run_copy()
    finally:
        factory_json.write_bytes(original_json)


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

    for label, mapping, engine_id, brake_id in (
        ("two-source profile A", profile_a, 256, 145),
        ("alternate CAN layout profile B", profile_b, 512, 146),
        ("empty profile", empty, None, None),
    ):
        print(f"\n== {label} ==", flush=True)
        configure_case(args, mapping, engine_id, brake_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
