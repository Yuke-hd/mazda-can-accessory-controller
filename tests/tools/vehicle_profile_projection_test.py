"""Independent consumer artifacts and real C++ catalog/export boundary checks."""

import copy
from dataclasses import replace
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from vehicle_profiles.canonical import identify_profile
from vehicle_profiles.projections import CATALOG_LIMITS, catalog_only_compiler_json, project_profile
from vehicle_profiles.validation import HandlerPolicy, ProfileValidationError, ValidationPolicies

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles"


def policies():
    return ValidationPolicies(
        {"synthetic.river": ("river.v1", "synthetic.v1"),
         "synthetic.desert": ("desert.v3", "desert-recognition.v2")},
        {("synthetic.ready", "v1"): HandlerPolicy(frozenset({"synthetic.river"}), "boolean", "", 0, 0)},
        frozenset({("synthetic/river-review", "Reference", "source_only")}),
        frozenset({"", "km/h"}), {("km/h", "km/h"): ("1", "0")}, frozenset({"unavailable"}), {})


class ProfileProjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        # Use the exact core fetched by an ordinary no-tests configure. The
        # real catalog and exporter compile below; no replica of their checks.
        configured = subprocess.run(["cmake", "-S", str(ROOT), "-B", str(cls.root / "build"),
            "-DBUILD_TESTING=OFF", "-DMAZDA_BUILD_HOST_TESTS=OFF"], capture_output=True, text=True)
        if configured.returncode:
            raise AssertionError(configured.stdout + configured.stderr)
        cache = (cls.root / "build/CMakeCache.txt").read_text()
        cls.core = Path(next(line.split("=", 1)[1] for line in cache.splitlines()
                            if line.startswith("MAZDA_VEHICLE_CAN_CORE_ROOT:PATH=")))
        cls.compiler = shutil.which("c++")
        if not cls.compiler:
            raise AssertionError("C++17 compiler is required for profile projection verification")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.profile = json.loads((FIXTURES / "synthetic-river.json").read_text())
        self.registry = policies()

    def project(self, profile=None, registry=None):
        return project_profile(self.profile if profile is None else profile,
                               policies=self.registry if registry is None else registry,
                               generator_revision="projection.test.v1")

    def probe(self, artifacts, bounds=False):
        (self.root / "generated_profile.hpp").write_bytes(artifacts.metadata_header)
        executable = self.root / "probe"
        result = subprocess.run([self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I", str(self.root), "-I", str(ROOT / "lib/vehicle_signals/include"),
            "-I", str(ROOT / "lib/companion_protocol/include"),
            "-I", str(self.core / "components/vehicle_core/include"),
            str(ROOT / "tests/tools/vehicle_profile_catalog_probe.cpp"),
            str(ROOT / "lib/companion_protocol/src/signal_catalog_export.cpp"), "-o", str(executable)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)] + (["bounds"] if bounds else []), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def test_independent_populated_and_empty_artifacts(self):
        for name in ("river", "empty"):
            with self.subTest(profile=name):
                profile = json.loads((FIXTURES / f"synthetic-{name}.json").read_text())
                expected = json.loads((FIXTURES / f"projection-{name}-expected.json").read_text())
                actual = self.project(profile)
                self.assertEqual(json.loads(actual.compiler_catalog), expected["compiler_catalog"])
                report = json.loads(actual.capability_report)
                identity = identify_profile(profile, policies=self.registry,
                    generator_revision="projection.test.v1", validation_limits=CATALOG_LIMITS).profile
                for key in ("artifact_identity", "compatibility_identity"):
                    self.assertEqual(report.pop(key), identity[key])
                self.assertEqual(report, expected["capability_report"])
                lines = self.probe(actual)
                self.assertEqual(lines[0], f'{len(expected["metadata_lines"])}\t{report["catalog_document_bytes"]}')
                self.assertEqual(lines[1:], expected["metadata_lines"])
                self.assertEqual(actual, self.project(profile))

    def native(self, count=1, enum=False):
        profile = copy.deepcopy(self.profile)
        profile.update(sources=[], messages=[], diagnostics=[], bindings=[])
        example = copy.deepcopy(self.profile["bindings"][1])
        for i in range(count):
            binding = copy.deepcopy(example)
            binding.update(key=f"signal.{i}", runtime_id=i + 1)
            if enum:
                binding.update(type="enum", choices=[{"key": f"c{j:02d}" + "x" * 60, "value": j} for j in range(32)])
                binding["policy"]["handler"] = "synthetic.enum"
            profile["bindings"].append(binding)
        registry = replace(self.registry, handlers={**self.registry.handlers,
            ("synthetic.enum", "v1"): HandlerPolicy(frozenset({"synthetic.river"}), "enum", "", 0, 0)})
        return profile, registry

    def test_existing_cpp_exporter_capacity_boundaries(self):
        self.probe(self.project(), bounds=True)
        profile, registry = self.native(64)
        self.assertEqual(self.probe(self.project(profile, registry))[0].split("\t")[0], "64")
        for count in (65,):
            profile, registry = self.native(count)
            with self.assertRaises(ProfileValidationError):
                self.project(profile, registry)
        profile, registry = self.native(enum=True)
        self.assertEqual(self.probe(self.project(profile, registry))[0].split("\t")[0], "1")
        profile["bindings"][0]["choices"].append({"key": "extra", "value": 32})
        with self.assertRaises(ProfileValidationError):
            self.project(profile, registry)
        profile, registry = self.native()
        profile["bindings"][0]["key"] = "a" * 63
        self.probe(self.project(profile, registry))
        profile["bindings"][0]["key"] += "a"
        with self.assertRaises(ProfileValidationError):
            self.project(profile, registry)

    def test_native_enum_provider_values_and_persistent_choice_keys(self):
        profile, registry = self.native(enum=True)
        profile["bindings"][0]["choices"] = [{"key": "stopped", "value": 65535},
                                                 {"key": "moving", "value": 7}]
        first = self.project(profile, registry)
        self.assertEqual(json.loads(first.compiler_catalog)["signals"]["signal.0"]["choices"],
                         ["moving", "stopped"])
        self.assertEqual(self.probe(first)[1], "1\tsignal.0\t2\t0\t0\t1\t7:moving\t65535:stopped")
        profile["bindings"][0]["choices"].reverse()
        self.assertEqual(first, self.project(profile, registry))

    def test_exact_document_bound_without_truncation(self):
        profile, registry = self.native(8, enum=True)
        # Independent v1 encoding: 3-byte document header, 38 bytes per
        # 8-character signal descriptor, 5-byte enum TLV/count and 3+key bytes
        # per choice. Reduce 859 bytes from maximum choices to reach 16384.
        reduction = 859
        for binding in profile["bindings"]:
            for choice in binding["choices"]:
                remove = min(reduction, 60)
                choice["key"] = choice["key"][:63 - remove]
                reduction -= remove
        self.assertEqual(reduction, 0)
        artifacts = self.project(profile, registry)
        self.assertEqual(self.probe(artifacts)[0], "8\t16384")
        profile["bindings"][0]["choices"][0]["key"] += "x"
        with self.assertRaisesRegex(ProfileValidationError, "document capacity"):
            self.project(profile, registry)

    def test_reordered_inputs_and_untrusted_prose_are_data(self):
        first = self.project()
        self.profile["sources"].reverse()
        self.profile["bindings"].reverse()
        self.profile["messages"][0]["fields"].reverse()
        self.assertEqual(first, self.project())
        self.profile["diagnostics"][0]["reason"] = '"; #include "evil.hpp"\n\\\u2603'
        second = self.project()
        self.assertEqual(second.metadata_header, first.metadata_header)
        self.assertIn("evil.hpp", json.loads(second.capability_report)["diagnostics"][0]["reason"])
        self.assertNotIn(b"evil.hpp", second.metadata_header)
        self.probe(second)

    def test_unrepresentable_units_or_confidence_fail_explicitly(self):
        profile, registry = self.native()
        profile["bindings"][0]["evidence"]["confidence"] = "Hypothesis"
        registry = replace(registry, evidence=registry.evidence | {("synthetic/river-review", "Hypothesis", "source_only")})
        with self.assertRaisesRegex(ProfileValidationError, "ValidationStatus"):
            self.project(profile, registry)
        profile = copy.deepcopy(self.profile)
        profile["bindings"][0]["unit"] = "mph"
        profile["bindings"][0]["policy"]["conversion"]["target_unit"] = "mph"
        registry = replace(self.registry, units=self.registry.units | {"mph"},
            unit_conversions={**self.registry.unit_conversions, ("km/h", "mph"): ("1", "0")})
        with self.assertRaisesRegex(ProfileValidationError, "SignalUnit"):
            self.project(profile, registry)

    def test_mazda_catalog_only_adapter_preserves_independent_baseline(self):
        baseline = json.loads((ROOT / "tools/controller_signal_catalog.json").read_text())
        projected = json.loads(catalog_only_compiler_json(baseline["signals"]))
        self.assertEqual(projected["signals"], baseline["signals"])
        self.assertEqual(set(projected), {"version", "signals"})
        import compile_controller_config
        path = self.root / "compiler_catalog.json"
        path.write_bytes(self.project().compiler_catalog)
        with patch.object(compile_controller_config, "SIGNAL_CATALOG_PATH", path):
            self.assertEqual(set(compile_controller_config._load_signal_catalog()),
                             {"vehicle.speed", "vehicle.synthetic_ready"})


if __name__ == "__main__":
    unittest.main()
