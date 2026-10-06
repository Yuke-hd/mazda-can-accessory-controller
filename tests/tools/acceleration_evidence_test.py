#!/usr/bin/env python3
"""Regression checks for the reviewed 0x078 acceleration evidence record."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SIGNAL_EVIDENCE = ROOT / "docs/protocol/signal-evidence.md"
OPENDBC_PROVENANCE = ROOT / "docs/protocol/opendbc-provenance.md"


class AccelerationEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.signal_evidence = SIGNAL_EVIDENCE.read_text(encoding="utf-8")
        self.opendbc_provenance = OPENDBC_PROVENANCE.read_text(encoding="utf-8")
        heading = "## Candidate acceleration mapping: `BRAKE` (`0x078`)"
        self.assertIn(heading, self.signal_evidence)
        self.acceleration_section = self.signal_evidence.split(heading, 1)[1].split(
            "## Source names and semantic differences", 1
        )[0]
        self.normalized_acceleration_section = " ".join(self.acceleration_section.split())

    def test_upstream_mapping_is_exactly_pinned_and_attributed(self) -> None:
        self.assertIn("`BRAKE` (120 / `0x078`)", self.opendbc_provenance)
        self.assertIn(
            "`VEHICLE_ACC_X`: `5|13@0+ (0.01,-40)`, `[-40|40]`, m/s^2",
            self.opendbc_provenance,
        )
        self.assertIn(
            "`VEHICLE_ACC_Y`: `8|13@0+ (0.001,-4.096)`, `[-4.096|4.096]`, m/s^2",
            self.opendbc_provenance,
        )
        self.assertIn(
            "95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L356-L360",
            self.opendbc_provenance,
        )

    def test_signal_evidence_keeps_axis_and_sign_at_reference_confidence(self) -> None:
        self.assertIn("`VEHICLE_ACC_X`", self.acceleration_section)
        self.assertIn("`VEHICLE_ACC_Y`", self.acceleration_section)
        self.assertIn("5|13@0+ (0.01,-40)", self.acceleration_section)
        self.assertIn("8|13@0+ (0.001,-4.096)", self.acceleration_section)
        self.assertIn("wrt. NED frame", self.acceleration_section)
        self.assertIn("**Reference**", self.acceleration_section)
        self.assertIn("longitudinal", self.acceleration_section)
        self.assertIn("lateral", self.acceleration_section)
        self.assertIn("sign convention", self.acceleration_section)

    def test_existing_capture_limits_and_minimal_validation_sequence_are_recorded(self) -> None:
        self.assertIn("existing CX-5 capture contains `0x078`", self.acceleration_section)
        self.assertIn("pressure-correlated `BrakePressureRaw`", self.acceleration_section)
        self.assertIn("does not establish either acceleration axis", self.acceleration_section)
        for step in (
            "stationary baseline",
            "straight acceleration",
            "straight braking",
            "left turn",
            "right turn",
        ):
            self.assertIn(step, self.acceleration_section)
        self.assertIn(
            "Synthetic vectors do not promote these candidates",
            self.normalized_acceleration_section,
        )


if __name__ == "__main__":
    unittest.main()
