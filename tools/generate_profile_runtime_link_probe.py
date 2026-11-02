#!/usr/bin/env python3
"""Generate profile runtime headers for tests and firmware resource fixtures."""

import argparse
import json
from pathlib import Path

from vehicle_profiles.canonical import identify_profile
from vehicle_profiles.projections import CATALOG_LIMITS, project_runtime_profile
from vehicle_profiles.runtime_codegen import RUNTIME_LIMITS
from vehicle_profiles.validation import HandlerPolicy, ValidationPolicies

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles"


def policies() -> ValidationPolicies:
    return ValidationPolicies(
        {
            "synthetic.river": ("river.v1", "synthetic.v1"),
            "synthetic.desert": ("desert.v3", "desert-recognition.v2"),
            "synthetic.runtime_limits": (
                "runtime-limits.v1",
                "runtime-limits-recognition.v1",
            ),
        },
        {
            ("synthetic.ready", "v1"): HandlerPolicy(
                frozenset({"synthetic.river"}), "boolean", "", 0, 0
            )
        },
        frozenset(
            {
                ("synthetic/river-review", "Reference", "source_only"),
                ("synthetic/runtime-timing", "Reference", "operational"),
                ("synthetic/runtime-limits", "Reference", "source_only"),
            }
        ),
        frozenset({"", "km/h"}),
        {("", ""): ("1", "0"), ("km/h", "km/h"): ("1", "0")},
        frozenset({"unavailable"}),
        {
            ("synthetic.river", "vehicle.number"): (
                500,
                "reviewed_operational",
                "synthetic/runtime-timing",
            )
        },
    )


def generate(selection: str, output_dir: Path, *, application_artifacts: bool = False) -> None:
    fixture = {
        "empty": "synthetic-empty.json",
        "populated": "synthetic-river.json",
        "max": "runtime-limits.json",
    }[selection]
    profile = json.loads((FIXTURES / fixture).read_text(encoding="utf-8"))
    if application_artifacts and selection != "max":
        raise ValueError("application artifacts are supported only for the maximum fixture")
    if selection == "populated":
        # The source fixture also demonstrates the deferred native-handler
        # catalog projection. R6 retains only its supported DBC decoder.
        profile["bindings"] = [
            binding for binding in profile["bindings"] if binding["kind"] == "dbc"
        ]
    generator_revision = "runtime-link-probe.r1"
    if application_artifacts:
        limits = {**CATALOG_LIMITS, **RUNTIME_LIMITS}
        profile = identify_profile(
            profile,
            policies=policies(),
            generator_revision=generator_revision,
            validation_limits=limits,
        ).profile
    projection = project_runtime_profile(
        profile, policies=policies(), generator_revision=generator_revision
    )
    if projection.runtime_header is None:
        raise ValueError("runtime header was not generated")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "vehicle_profile_metadata.hpp").write_bytes(projection.metadata_header)
    (output_dir / "vehicle_profile_runtime.hpp").write_bytes(projection.runtime_header)
    (output_dir / "capability_report.json").write_bytes(projection.capability_report)
    if application_artifacts:
        (output_dir / "controller_signal_catalog.json").write_bytes(projection.compiler_catalog)
        (output_dir / "vehicle_profile.json").write_text(
            json.dumps(profile, ensure_ascii=True, sort_keys=True, indent=2, allow_nan=False) + "\n",
            encoding="utf-8",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", choices=("empty", "populated", "max"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--application-artifacts",
        action="store_true",
        help="also emit the selected profile JSON and compiler catalog for firmware fixture builds",
    )
    args = parser.parse_args()
    generate(args.selection, args.output_dir, application_artifacts=args.application_artifacts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
