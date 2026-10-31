"""Compile generated runtime functions and invoke them through dispatch."""

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles"
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.canonical import identify_profile
from vehicle_profiles.projections import CATALOG_LIMITS, project_runtime_profile
from vehicle_profiles.runtime_codegen import RUNTIME_LIMITS
from vehicle_profiles.validation import HandlerPolicy, ValidationPolicies, ProfileValidationError


def policies():
    return ValidationPolicies(
        {
            "synthetic.river": ("river.v1", "synthetic.v1"),
            "synthetic.desert": ("desert.v3", "desert-recognition.v2"),
        },
        {("synthetic.ready", "v1"): HandlerPolicy(frozenset({"synthetic.river"}), "boolean", "", 0, 0)},
        frozenset({
            ("synthetic/river-review", "Reference", "source_only"),
            ("synthetic/runtime-timing", "Reference", "operational"),
        }),
        frozenset({"", "km/h"}),
        {("", ""): ("1", "0"), ("km/h", "km/h"): ("1", "0")},
        frozenset({"unavailable"}),
        {("synthetic.river", "vehicle.number"): (500, "reviewed_operational", "synthetic/runtime-timing")},
    )


def field(field_id, *, start, length, byte_order="intel", signed=False,
          scale="1", offset="0", minimum="0", maximum="1", unit="", value_table=None):
    return {
        "id": field_id,
        "name": field_id,
        "sources": ["river-a"],
        "start": start,
        "length": length,
        "byte_order": byte_order,
        "signed": signed,
        "scale": scale,
        "offset": offset,
        "minimum": minimum,
        "maximum": maximum,
        "unit": unit,
        "value_table": [] if value_table is None else value_table,
    }


def binding(key, runtime_id, value_type, unit, field_id, *, choices=None,
            enum_map=None, invalid_ranges=None, notify=False, freshness=None):
    precision = {"number": "float32", "boolean": "boolean", "enum": "uint16"}[value_type]
    conversion = {
        "source_unit": unit,
        "target_unit": unit,
        "scale": "1",
        "offset": "0",
        "target_precision": precision,
        "invalid_ranges": invalid_ranges or [],
    }
    return {
        "key": key,
        "runtime_id": runtime_id,
        "type": value_type,
        "unit": unit,
        "capabilities": ["read", "notify"] if notify else ["read"],
        "choices": choices or [],
        "kind": "dbc",
        "inputs": [field_id],
        "policy": {"conversion": conversion, "enum_map": enum_map or [], "invalid_value": "unavailable"},
        "evidence": {"ref": "synthetic/river-review", "confidence": "Reference", "eligibility": "source_only"},
        "freshness": freshness,
    }


def populated_profile():
    profile = json.loads((FIXTURES / "synthetic-river.json").read_text())
    number_field = "runtime.raw_number"
    boolean_field = "runtime.raw_boolean"
    enum_field = "runtime.raw_enum"
    profile["messages"] = [
        {"id": "runtime.number", "bus": "body", "identifier_format": "standard", "identifier": 0x100,
         "dlc": 8, "fields": [field(number_field, start=0, length=64,
             scale="0.250000000000000000001",
             offset="-2305843009213693952.009223372036854775808",
             minimum="0", maximum="25.0000000000000000001", unit="km/h")]},
        {"id": "runtime.boolean", "bus": "body", "identifier_format": "standard", "identifier": 0x101,
         "dlc": 1, "fields": [field(boolean_field, start=0, length=1)]},
        {"id": "runtime.enum", "bus": "body", "identifier_format": "standard", "identifier": 0x102,
         "dlc": 1, "fields": [field(enum_field, start=7, length=2, byte_order="motorola", maximum="3",
             value_table=[{"raw": "0", "label": "stopped"}, {"raw": "1", "label": "moving"}])]},
    ]
    profile["bindings"] = [
        binding("vehicle.boolean", 1, "boolean", "", boolean_field),
        binding("vehicle.enum", 2, "enum", "", enum_field,
                choices=[{"key": "stopped", "value": 65535}, {"key": "moving", "value": 7}],
                enum_map=[{"raw": "0", "choice": "stopped"}, {"raw": "1", "choice": "moving"}]),
        binding("vehicle.number", 3, "number", "km/h", number_field, notify=True,
                invalid_ranges=[{"minimum": "12.5", "maximum": "12.6"}],
                freshness={"timeout_ms": 500, "basis": "reviewed_operational",
                           "evidence_ref": "synthetic/runtime-timing"}),
    ]
    profile["diagnostics"] = []
    return profile


class RuntimeCodeGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        configured_core = os.environ.get("MAZDA_VEHICLE_CAN_CORE_ROOT")
        if configured_core:
            cls.core = Path(configured_core)
        else:
            configured = subprocess.run(
                ["cmake", "-S", str(ROOT), "-B", str(cls.root / "build"),
                 "-DBUILD_TESTING=OFF", "-DMAZDA_BUILD_HOST_TESTS=OFF"],
                capture_output=True, text=True)
            if configured.returncode:
                raise AssertionError(configured.stdout + configured.stderr)
            cache = (cls.root / "build/CMakeCache.txt").read_text()
            cls.core = Path(next(line.split("=", 1)[1] for line in cache.splitlines()
                                 if line.startswith("MAZDA_VEHICLE_CAN_CORE_ROOT:PATH=")))
        if not cls.core.is_dir():
            raise AssertionError(f"vehicle CAN core source is missing: {cls.core}")
        cls.compiler = shutil.which("c++")
        if not cls.compiler:
            raise AssertionError("C++17 compiler is required for profile runtime verification")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def compile_and_run(self, projections, *, empty=False, negative=False,
                        small_scale_wide=False, signed_negative_scale=False):
        generated = self.root / ("empty" if empty else "populated")
        generated.mkdir(exist_ok=True)
        (generated / "vehicle_profile_metadata.hpp").write_bytes(projections.metadata_header)
        self.assertIsNotNone(projections.runtime_header)
        (generated / "vehicle_profile_runtime.hpp").write_bytes(projections.runtime_header)
        executable = self.root / ("probe-empty" if empty else "probe-populated")
        command = [
            self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I", str(generated),
            "-I", str(ROOT / "lib/vehicle_profile_runtime/include"),
            "-I", str(ROOT / "lib/vehicle_signals/include"),
            "-I", str(self.core / "components/vehicle_core/include"),
        ]
        if empty:
            command.append("-DPROFILE_EMPTY=1")
        if negative:
            command.append("-DPROFILE_NEGATIVE_SCALE=1")
        if small_scale_wide:
            command.append("-DPROFILE_SMALL_SCALE_WIDE=1")
        if signed_negative_scale:
            command.append("-DPROFILE_SIGNED_NEGATIVE_SCALE=1")
        command += [
            str(ROOT / "tests/tools/vehicle_profile_runtime_probe.cpp"),
            str(ROOT / "lib/vehicle_profile_runtime/src/extract.cpp"),
            "-o", str(executable),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def project(self, profile, registry=None):
        return project_runtime_profile(
            profile,
            policies=policies() if registry is None else registry,
            generator_revision="runtime.test.v1",
        )

    def test_generated_number_boolean_enum_and_dispatch_match_independent_vectors(self):
        profile = populated_profile()
        projections = self.project(profile)
        self.compile_and_run(projections)
        expected = identify_profile(
            profile, policies=policies(), generator_revision="runtime.test.v1",
            validation_limits={**CATALOG_LIMITS, **RUNTIME_LIMITS},
        ).profile
        report = json.loads(projections.capability_report)
        self.assertEqual(report["artifact_identity"], expected["artifact_identity"])
        self.assertIn(b"kLimitSelectedMessages = 64U", projections.runtime_header)
        self.assertIn(b"kLimitExtractedFields = 64U", projections.runtime_header)

    def test_empty_profile_has_compilable_empty_tables(self):
        profile = json.loads((FIXTURES / "synthetic-empty.json").read_text())
        projections = self.project(profile)
        self.compile_and_run(projections, empty=True)

    def test_negative_source_scale_reverses_exact_raw_intervals(self):
        profile = populated_profile()
        number_field = profile["messages"][0]["fields"][0]
        number_field.update(scale="-0.25", offset="2305843009213693952",
                            minimum="-25", maximum="0")
        profile["bindings"][2]["policy"]["conversion"]["invalid_ranges"] = [
            {"minimum": "-12.6", "maximum": "-12.5"},
        ]
        self.compile_and_run(self.project(profile), negative=True)

    def test_wide_u64_small_scale_preserves_zero_and_unit_increment(self):
        profile = populated_profile()
        number_field = profile["messages"][0]["fields"][0]
        number_field.update(scale="0.1", offset="0", minimum="0",
                            maximum="1844674407370955161.5")
        self.compile_and_run(self.project(profile), small_scale_wide=True)

    def test_signed64_extrema_and_negative_scale_invalid_interval(self):
        profile = populated_profile()
        number_field = profile["messages"][0]["fields"][0]
        number_field.update(signed=True, scale="-0.25", offset="0",
                            minimum="-2305843009213693951.75",
                            maximum="2305843009213693952")
        profile["bindings"][2]["policy"]["conversion"]["invalid_ranges"] = [
            {"minimum": "-0.5", "maximum": "0"},
        ]
        self.compile_and_run(self.project(profile), signed_negative_scale=True)

    def test_reordering_preserves_generated_bytes_and_identities(self):
        profile = populated_profile()
        first = self.project(profile)
        reordered = copy.deepcopy(profile)
        reordered["sources"].reverse()
        reordered["messages"].reverse()
        for message in reordered["messages"]:
            message["fields"].reverse()
        reordered["bindings"].reverse()
        self.assertEqual(first, self.project(reordered))

    def test_native_handler_and_unsupported_selected_wire_form_are_rejected(self):
        native = json.loads((FIXTURES / "synthetic-river.json").read_text())
        with self.assertRaisesRegex(ProfileValidationError, "native handlers"):
            self.project(native)

        unsupported = populated_profile()
        unsupported["messages"][0]["identifier_format"] = "extended"
        unsupported["messages"][0]["identifier"] = 0x12345
        with self.assertRaisesRegex(ProfileValidationError, "standard classic CAN"):
            self.project(unsupported)

    def test_notify_cap_violation_is_rejected(self):
        profile = populated_profile()
        base = copy.deepcopy(profile["bindings"][2])
        for index in range(18):
            extra = copy.deepcopy(base)
            extra.update(key=f"vehicle.extra_{index:02d}", runtime_id=10 + index,
                         capabilities=["read", "notify"], freshness=None)
            profile["bindings"].append(extra)
        with self.assertRaisesRegex(ProfileValidationError, "max_runtime_notify_bindings"):
            self.project(profile)


if __name__ == "__main__":
    unittest.main()
