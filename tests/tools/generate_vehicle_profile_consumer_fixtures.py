#!/usr/bin/env python3
"""Generate build-only decoder tables for the generic consumer acceptance test."""

import argparse
import copy
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "tools"))

from vehicle_profile_runtime_generation_test import binding, field, policies, populated_profile
from vehicle_profiles.projections import project_runtime_profile


def source_field(field_id: str, source_id: str, **values):
    result = field(field_id, **values)
    result["sources"] = [source_id]
    return result


def generated_profile(
    *,
    profile_id,
    semantic_revision,
    recognition_revision,
    selected_bus,
    source_id,
    source_path,
    source_sha256,
    message_base,
    runtime_id_base,
    namespace,
    scale,
    enum_rows,
    number_freshness,
):
    profile = copy.deepcopy(populated_profile())
    profile["profile_id"] = profile_id
    profile["semantic_policy_revision"] = semantic_revision
    profile["recognition_rule_revision"] = recognition_revision
    profile["selected_bus"] = selected_bus
    profile["sources"] = [
        {
            "id": source_id,
            "kind": "dbc",
            "path": source_path,
            "sha256": source_sha256,
            "revision": semantic_revision,
            "license": "Apache-2.0; independently authored synthetic data",
            "bus": selected_bus,
        }
    ]

    bool_field = f"{namespace}.raw_boolean"
    number_field = f"{namespace}.raw_number"
    enum_field = f"{namespace}.raw_enum"
    enum_value_table = [{"raw": str(raw), "label": key} for raw, key, _ in enum_rows]
    profile["messages"] = [
        {
            "id": f"{namespace}.boolean",
            "bus": selected_bus,
            "identifier_format": "standard",
            "identifier": message_base + 1,
            "dlc": 1,
            "fields": [
                source_field(bool_field, source_id, start=0, length=1),
            ],
        },
        {
            "id": f"{namespace}.number",
            "bus": selected_bus,
            "identifier_format": "standard",
            "identifier": message_base + 2,
            "dlc": 1,
            "fields": [
                source_field(
                    number_field,
                    source_id,
                    start=0,
                    length=8,
                    scale=scale,
                    minimum="0",
                    maximum=str(255 * int(scale)),
                    unit="km/h",
                ),
            ],
        },
        {
            "id": f"{namespace}.enum",
            "bus": selected_bus,
            "identifier_format": "standard",
            "identifier": message_base + 3,
            "dlc": 1,
            "fields": [
                source_field(
                    enum_field,
                    source_id,
                    start=0,
                    length=2,
                    maximum="3",
                    value_table=enum_value_table,
                ),
            ],
        },
    ]
    profile["bindings"] = [
        binding(
            "vehicle.boolean" if profile_id == "synthetic.river" else "profile_b.active",
            runtime_id_base,
            "boolean",
            "",
            bool_field,
            notify=profile_id != "synthetic.river",
        ),
        binding(
            "vehicle.number" if profile_id == "synthetic.river" else "profile_b.level",
            runtime_id_base + 1,
            "number",
            "km/h",
            number_field,
            notify=number_freshness is not None,
            freshness=number_freshness,
        ),
        binding(
            "vehicle.enum" if profile_id == "synthetic.river" else "profile_b.mode",
            runtime_id_base + 2,
            "enum",
            "",
            enum_field,
            choices=[{"key": key, "value": value} for _, key, value in enum_rows],
            enum_map=[{"raw": str(raw), "choice": key} for raw, key, _ in enum_rows],
        ),
    ]
    profile["diagnostics"] = []
    return profile


def emit(output_dir: Path, name: str, profile) -> None:
    projected = project_runtime_profile(
        profile,
        policies=policies(),
        generator_revision="runtime-consumer.acceptance.v1",
    )
    directory = output_dir / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "vehicle_profile_metadata.hpp").write_bytes(projected.metadata_header)
    if projected.runtime_header is None:
        raise RuntimeError(f"{name} did not produce a runtime header")
    (directory / "vehicle_profile_runtime.hpp").write_bytes(projected.runtime_header)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    profile_a = generated_profile(
        profile_id="synthetic.river",
        semantic_revision="river.v1",
        recognition_revision="synthetic.v1",
        selected_bus="body",
        source_id="river-a",
        source_path="synthetic/river-a.dbc",
        source_sha256="a" * 64,
        message_base=0x100,
        runtime_id_base=101,
        namespace="runtime_a",
        scale="1",
        enum_rows=[(0, "moving", 5), (1, "stopped", 7)],
        number_freshness={
            "timeout_ms": 500,
            "basis": "reviewed_operational",
            "evidence_ref": "synthetic/runtime-timing",
        },
    )
    profile_b = generated_profile(
        profile_id="synthetic.desert",
        semantic_revision="desert.v3",
        recognition_revision="desert-recognition.v2",
        selected_bus="powertrain",
        source_id="desert-b",
        source_path="synthetic/desert-b.dbc",
        source_sha256="b" * 64,
        message_base=0x200,
        runtime_id_base=201,
        namespace="runtime_b",
        scale="3",
        enum_rows=[(2, "park", 11), (3, "drive", 22)],
        number_freshness=None,
    )
    empty = ROOT / "tests" / "fixtures" / "vehicle-profiles" / "synthetic-empty.json"

    import json

    emit(args.output_dir, "profile_a", profile_a)
    emit(args.output_dir, "profile_b", profile_b)
    emit(args.output_dir, "profile_empty", json.loads(empty.read_text(encoding="utf-8")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
