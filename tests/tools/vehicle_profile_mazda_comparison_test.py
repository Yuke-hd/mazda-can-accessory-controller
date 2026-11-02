"""Compare generated Mazda DBC decoding with native decoding on synthetic frames."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/vehicle-profiles/mazda-comparison"
MAPPING = FIXTURE / "mapping.json"
VECTORS = FIXTURE / "vectors.json"


class MazdaDecoderComparisonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="mazda-profile-comparison-")
        cls.root = Path(cls.temporary.name)
        cls.vectors = json.loads(VECTORS.read_text(encoding="utf-8"))

        source = ROOT / "docs/protocol/mazda_custom.dbc"
        source_bytes = source.read_bytes()
        source_hash = hashlib.sha256(source_bytes).hexdigest()
        if source_hash != cls.vectors["source"]["sha256"]:
            raise AssertionError(f"reviewed Mazda DBC digest changed: {source_hash}")

        mapping = json.loads(MAPPING.read_text(encoding="utf-8"))
        if not mapping["sources"][0]["license"].startswith(
            "No separate license is recorded for this DBC;"
        ):
            raise AssertionError("the mapping must state the known DBC license status")
        source_confidence = cls.vectors["source"]["confidence"]
        for binding in mapping["bindings"]:
            source_key = f"{binding['message']}.{binding['field']}"
            if source_key not in source_confidence:
                raise AssertionError(f"missing source confidence for {source_key}")
        cls.input_dir = cls.root / "input"
        cls.input_dir.mkdir()
        shutil.copyfile(source, cls.input_dir / "mazda_custom.dbc")
        shutil.copyfile(MAPPING, cls.input_dir / "mapping.json")

        imported = subprocess.run(
            [sys.executable, str(ROOT / "tools/import_vehicle_profile.py"),
             "--mapping", str(cls.input_dir / "mapping.json"),
             "--output-dir", str(cls.root / "artifacts"), "--print-directory"],
            cwd=ROOT, capture_output=True, text=True,
        )
        if imported.returncode:
            raise AssertionError(imported.stdout + imported.stderr)
        cls.generated = Path(imported.stdout.strip())

        source_report = json.loads((cls.generated / "source_report.json").read_text())
        profile = json.loads((cls.generated / "vehicle_profile.json").read_text())
        policies = json.loads((cls.generated / "validation_policies.json").read_text())
        source_record = source_report["sources"][0]
        cls.assert_source_hash = source_record["sha256"]
        if source_record["version"] != cls.vectors["source"]["version"]:
            raise AssertionError(f"unexpected Mazda DBC version: {source_record['version']}")
        if cls.assert_source_hash != source_hash:
            raise AssertionError("imported Mazda source digest does not match the reviewed bytes")
        if profile["sources"][0]["license"] != mapping["sources"][0]["license"]:
            raise AssertionError("imported profile lost the mapping's license/provenance note")
        if policies["handlers"] or policies["freshness_policies"]:
            raise AssertionError("comparison profile must have no native handlers or freshness policy")
        if any(binding["evidence"]["confidence"] != "user_mapped"
               for binding in profile["bindings"]):
            raise AssertionError("imported comparison bindings must retain user-mapped confidence")
        if any(binding["evidence"]["eligibility"] != "source_only"
               for binding in profile["bindings"]):
            raise AssertionError("imported comparison bindings must remain source-only")
        if any(binding["key"] == "vehicle.brake_pressed" for binding in profile["bindings"]):
            raise AssertionError("the Mazda-only brake exception must not enter an imported mapping")
        generated_json = json.dumps({"profile": profile, "policies": policies})
        if "fresh_or_unverified" in generated_json:
            raise AssertionError("imported comparison profile contains the Mazda brake opt-in")

        configured_core = os.environ.get("MAZDA_VEHICLE_CAN_CORE_ROOT")
        if configured_core:
            cls.core = Path(configured_core)
        else:
            configured = subprocess.run(
                ["cmake", "-S", str(ROOT), "-B", str(cls.root / "core-build"),
                 "-DBUILD_TESTING=OFF", "-DMAZDA_BUILD_HOST_TESTS=OFF"],
                capture_output=True, text=True,
            )
            if configured.returncode:
                raise AssertionError(configured.stdout + configured.stderr)
            cache = (cls.root / "core-build/CMakeCache.txt").read_text()
            cls.core = Path(next(line.split("=", 1)[1] for line in cache.splitlines()
                                 if line.startswith("MAZDA_VEHICLE_CAN_CORE_ROOT:PATH=")))
        if not cls.core.is_dir():
            raise AssertionError(f"vehicle CAN core source is missing: {cls.core}")

        compiler = shutil.which("c++")
        if compiler is None:
            raise AssertionError("C++17 compiler is required for Mazda decoder comparison")
        cls.executable = cls.root / "mazda-comparison-probe"
        command = [
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I", str(cls.generated),
            "-I", str(ROOT / "lib/vehicle_profile_runtime/include"),
            "-I", str(ROOT / "lib/vehicle_signals/include"),
            "-I", str(ROOT / "lib/mazda/include"),
            "-I", str(ROOT / "lib/mazda/internal_include"),
            "-I", str(ROOT / "lib/signal_notification/include"),
            "-I", str(cls.core / "components/vehicle_core/include"),
            str(ROOT / "tests/tools/vehicle_profile_mazda_comparison_probe.cpp"),
            str(ROOT / "lib/vehicle_profile_runtime/src/extract.cpp"),
            str(ROOT / "lib/mazda/src/mazda_candidate.cpp"),
            str(ROOT / "lib/mazda/src/state.cpp"),
            str(cls.core / "components/vehicle_core/src/vehicle_core.cpp"),
            "-o", str(cls.executable),
        ]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

        cls.choice_values = {
            binding["key"].rsplit(".", 1)[-1]: {
                choice["key"]: choice["value"] for choice in binding["choices"]
            }
            for binding in profile["bindings"] if binding["choices"]
        }

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_generated_and_native_decoders_match_independent_synthetic_vectors(self):
        lines = []
        for index, vector in enumerate(self.vectors["vectors"]):
            payload = " ".join(str(value) for value in vector["data"])
            lines.append(f"{index} {vector['identifier']} {payload}")
        decoded = subprocess.run(
            [str(self.executable)], input="\n".join(lines) + "\n",
            capture_output=True, text=True,
        )
        self.assertEqual(decoded.returncode, 0, decoded.stdout + decoded.stderr)
        rows = [self._decode_row(line) for line in decoded.stdout.splitlines()]
        self.assertEqual(len(rows), len(self.vectors["vectors"]))

        for index, (vector, actual) in enumerate(zip(self.vectors["vectors"], rows)):
            with self.subTest(vector=vector["name"]):
                self.assertEqual(actual["index"], index)
                expected = vector["expected"]
                for implementation in ("generated", "native"):
                    self._assert_fields(expected[implementation], actual[implementation])

                expected_generated_fields = set(expected["generated"])
                actual_generated_fields = {
                    field for field, values in actual["generated"].items()
                    if values["present"]
                }
                self.assertEqual(actual_generated_fields, expected_generated_fields)
                for field in expected["generated"]:
                    self.assertTrue(actual["generated"][field]["present"],
                                    f"generated dispatch omitted {field}")
                for values in actual["generated"].values():
                    values.pop("present")

                intentional: dict[str, set[str]] = {
                    "engine_rpm_rejects_the_native_engine_data_message": {
                        "accelerator_pedal_position"},
                    "mapped_unknown_actual_gears_stay_valid_in_imported_profiles": {
                        "actual_gear"},
                    "mapped_unknown_selector_codes_stay_valid_in_imported_profiles": {
                        "selector_position"},
                }
                intentional_names = set(vector.get("intentional_differences", []))
                self.assertTrue(
                    intentional_names.issubset(intentional),
                    "fixture references an undocumented decoder difference",
                )
                intentional_fields = set().union(*(intentional[name] for name in intentional_names))

                expected_native_valid = {
                    field for field, values in expected["native"].items()
                    if field != "status" and values["valid"]
                }
                actual_native_valid = {
                    field for field, values in actual["native"].items()
                    if field != "status" and values["valid"]
                }
                self.assertEqual(actual_native_valid, expected_native_valid)
                for field in set(expected["generated"]) & set(expected["native"]):
                    if field in intentional_fields:
                        self.assertNotEqual(actual["generated"][field], actual["native"][field])
                    else:
                        self.assertEqual(actual["generated"][field], actual["native"][field])
                    generated_field = expected["generated"][field]
                    if "key" in generated_field:
                        self.assertEqual(self.choice_values[field][generated_field["key"]],
                                         generated_field["value"])

    def _assert_fields(self, expected, actual):
        if "status" in expected:
            if expected["status"] == "decoded":
                actual_status = actual.pop("status")
                if actual_status != "decoded":
                    raise AssertionError(f"expected decoded, got {actual_status}")
            else:
                actual_status = actual.pop("status")
                if actual_status != expected["status"]:
                    raise AssertionError(f"expected {expected['status']}, got {actual_status}")
        for field, expectation in expected.items():
            if field == "status":
                continue
            value = actual[field]
            if "key" in expectation:
                # Choice labels are checked against the catalog by their independent code.
                expectation = {key: member for key, member in expectation.items() if key != "key"}
            for member, expected_value in expectation.items():
                if member == "value" and isinstance(expected_value, float):
                    self.assertAlmostEqual(value[member], expected_value, places=6)
                else:
                    self.assertEqual(value[member], expected_value)

    @staticmethod
    def _decode_row(line):
        columns = line.split("\t")
        if len(columns) != 22:
            raise AssertionError(f"expected 22 probe columns, got {len(columns)}: {line}")
        index = int(columns[0])
        generated = {
            "engine_rpm": {"present": columns[1] == "1", "valid": columns[2] == "1",
                           "value": float(columns[3])},
            "accelerator_pedal_position": {"present": columns[4] == "1",
                                           "valid": columns[5] == "1",
                                           "value": float(columns[6])},
            "selector_position": {"present": columns[7] == "1", "valid": columns[8] == "1",
                                  "value": int(columns[9])},
            "actual_gear": {"present": columns[10] == "1", "valid": columns[11] == "1",
                            "value": int(columns[12])},
        }
        native = {
            "status": columns[13],
            "engine_rpm": {"valid": columns[14] == "1", "value": float(columns[15])},
            "accelerator_pedal_position": {"valid": columns[16] == "1",
                                           "value": float(columns[17])},
            "selector_position": {"valid": columns[18] == "1", "value": int(columns[19])},
            "actual_gear": {"valid": columns[20] == "1", "value": int(columns[21])},
        }
        return {"index": index, "generated": generated, "native": native}


if __name__ == "__main__":
    unittest.main()
