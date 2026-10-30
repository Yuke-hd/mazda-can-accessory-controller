#!/usr/bin/env python3
"""Generate build-tree headers for the test-only retained runtime probe."""

import argparse
import json
from pathlib import Path

from vehicle_profiles.projections import project_runtime_profile
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


def generate(selection: str, output_dir: Path) -> None:
    fixture = {
        "empty": "synthetic-empty.json",
        "populated": "synthetic-river.json",
        "max": "runtime-limits.json",
    }[selection]
    profile = json.loads((FIXTURES / fixture).read_text(encoding="utf-8"))
    if selection == "populated":
        # The source fixture also demonstrates the deferred native-handler
        # catalog projection. R6 retains only its supported DBC decoder.
        profile["bindings"] = [
            binding for binding in profile["bindings"] if binding["kind"] == "dbc"
        ]
    projection = project_runtime_profile(
        profile, policies=policies(), generator_revision="runtime-link-probe.r1"
    )
    if projection.runtime_header is None:
        raise ValueError("runtime header was not generated")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "vehicle_profile_metadata.hpp").write_bytes(projection.metadata_header)
    (output_dir / "vehicle_profile_runtime.hpp").write_bytes(projection.runtime_header)
    (output_dir / "capability_report.json").write_bytes(projection.capability_report)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", choices=("empty", "populated", "max"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    generate(args.selection, args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
