#!/usr/bin/env python3
"""Module-local positive and intentional-drift checks for the S2-D host tool."""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import compare_mazda_dbc  # noqa: E402

# CTest passes the module's already-built dumper so the registered test
# exercises the same target exposed to host users. Direct invocation without
# CMake still compiles the dumper in a temporary directory.
_metadata_dumper: Path | None = None
if "--metadata-dumper" in sys.argv:
    _metadata_dumper_index = sys.argv.index("--metadata-dumper")
    _metadata_dumper = Path(sys.argv[_metadata_dumper_index + 1])
    del sys.argv[_metadata_dumper_index : _metadata_dumper_index + 2]


class DbcMetadataComparisonTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dbc_path = ROOT / "docs/protocol/mazda_custom.dbc"
        self.dbc = compare_mazda_dbc.parse_dbc(self.dbc_path.read_text(encoding="utf-8"))
        self.metadata_text = (
            compare_mazda_dbc._run_existing_dumper(_metadata_dumper)
            if _metadata_dumper is not None
            else compare_mazda_dbc._run_dumper(ROOT, "c++")
        )
        self.metadata = compare_mazda_dbc.parse_metadata(self.metadata_text)

    def test_reviewed_dbc_matches_compiled_supported_metadata(self) -> None:
        failures = compare_mazda_dbc.compare(self.dbc, self.metadata)
        self.assertEqual(failures, [])
        self.assertEqual(len(self.metadata), 17)
        self.assertNotIn(("ENGINE_DATA", "SPEED"), self.metadata)

    def test_source_dbc_bytes_are_unchanged_reviewed_artifact(self) -> None:
        digest = hashlib.sha256(self.dbc_path.read_bytes()).hexdigest()
        self.assertEqual(digest, "26e8adcf2791ca3da047fae9404dbcde2c8858d840f0a85c062072223d69f31f")

    def test_decoder_start_bit_coordinates_follow_dbc_byte_order(self) -> None:
        # Motorola uses the DBC sawtooth coordinate for its most-significant
        # bit, while Intel's start is already the least-significant payload bit.
        self.assertEqual(
            compare_mazda_dbc.decoder_start_bit(self.dbc[("ENGINE_DATA", "EngineRPM")]),
            0,
        )
        self.assertEqual(
            compare_mazda_dbc.decoder_start_bit(self.dbc[("TRANSMISSION", "ActualGear")]),
            33,
        )
        self.assertEqual(
            compare_mazda_dbc.decoder_start_bit(
                self.dbc[("BLINK_INFO", "LeftIndicatorLamp_Reference")]
            ),
            18,
        )

    def test_intentional_numeric_drift_fails_with_numeric_diagnostic(self) -> None:
        drifted = self.metadata_text.replace(
            "ENGINE_DATA\tEngineRPM\tengine_rpm\tEngineRPM\t514\t0\t7\t16\t0.25\t",
            "ENGINE_DATA\tEngineRPM\tengine_rpm\tEngineRPM\t514\t0\t7\t16\t0.5\t",
            1,
        )
        drifted_metadata = compare_mazda_dbc.parse_metadata(drifted)
        failures = compare_mazda_dbc.compare(self.dbc, drifted_metadata)
        self.assertTrue(any("EngineRPM scale mismatch" in failure for failure in failures), failures)

    def test_intentional_decoder_bit_coordinate_drift_fails_with_diagnostic(self) -> None:
        drifted = self.metadata_text.replace(
            "ENGINE_DATA\tEngineRPM\tengine_rpm\tEngineRPM\t514\t0\t7\t16\t0.25\t",
            "ENGINE_DATA\tEngineRPM\tengine_rpm\tEngineRPM\t514\t8\t7\t16\t0.25\t",
            1,
        )
        drifted_metadata = compare_mazda_dbc.parse_metadata(drifted)
        failures = compare_mazda_dbc.compare(self.dbc, drifted_metadata)
        self.assertTrue(
            any(
                "EngineRPM decoder start bit mismatch: DBC 0 != metadata 8" in failure
                for failure in failures
            ),
            failures,
        )

    def test_intentional_channel_drift_fails_with_channel_diagnostic(self) -> None:
        drifted = self.metadata_text.replace(
            "ENGINE_DATA\tEngineRPM\tengine_rpm\t",
            "ENGINE_DATA\tEngineRPM\tdrifted_channel\t",
            1,
        )
        drifted_metadata = compare_mazda_dbc.parse_metadata(drifted)
        failures = compare_mazda_dbc.compare(self.dbc, drifted_metadata)
        self.assertTrue(any("EngineRPM channel mismatch" in failure for failure in failures), failures)

    def test_promoting_confidence_without_evidence_fails(self) -> None:
        promoted = self.metadata_text.replace(
            "TRANSMISSION\tActualGear\tactual_gear\tActualGear\t552\t33\t36\t4\t1\t0\t0\t15\tMotorola\tNone\tObserved\t",
            "TRANSMISSION\tActualGear\tactual_gear\tActualGear\t552\t33\t36\t4\t1\t0\t0\t15\tMotorola\tNone\tConfirmed\t",
            1,
        )
        promoted_metadata = compare_mazda_dbc.parse_metadata(promoted)
        failures = compare_mazda_dbc.compare(self.dbc, promoted_metadata)
        self.assertTrue(any("ActualGear confidence mismatch" in failure for failure in failures), failures)

    def test_cli_reports_intentional_drift_as_failure(self) -> None:
        drifted = self.metadata_text.replace("\t0.25\t", "\t0.5\t", 1)
        with tempfile.NamedTemporaryFile("w", encoding="utf-8") as metadata_file:
            metadata_file.write(drifted)
            metadata_file.flush()
            result = compare_mazda_dbc.main(
                [
                    "--root",
                    str(ROOT),
                    "--metadata-file",
                    metadata_file.name,
                ]
            )
        self.assertEqual(result, 1)


if __name__ == "__main__":
    unittest.main()
