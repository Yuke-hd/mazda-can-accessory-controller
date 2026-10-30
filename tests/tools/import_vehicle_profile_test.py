"""End-to-end tests for the mapping-driven DBC import command."""

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
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
TOOL = ROOT / "tools/import_vehicle_profile.py"

import import_vehicle_profile  # noqa: E402
from generate_vehicle_profile import load_policies  # noqa: E402
from vehicle_profiles import dbc_import  # noqa: E402
from vehicle_profiles.artifact_set import _write, publish  # noqa: E402


class ImportCommandTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.inputs = self.work / "inputs"
        shutil.copytree(FIXTURES, self.inputs)
        self.out = self.work / "out"

    def run_cli(self, *extra, out=None, mapping="mapping-demo-a.json"):
        command = [sys.executable, str(TOOL), "--mapping", str(self.inputs / mapping),
                   "--output-dir", str(out or self.out), *extra]
        return subprocess.run(command, capture_output=True, text=True)

    def current(self, out=None):
        root = out or self.out
        index = json.loads((root / "current.json").read_text())
        return root / index["directory"]

    def tree(self, directory):
        return {p.name: p.read_bytes() for p in sorted(directory.iterdir())}

    def test_publishes_complete_artifact_set(self):
        result = self.run_cli("--print-directory")
        self.assertEqual(result.returncode, 0, result.stderr)
        directory = self.current()
        self.assertEqual(result.stdout.strip(), str(directory.resolve()))
        names = set(self.tree(directory))
        self.assertEqual(names, {
            "controller_signal_catalog.json", "vehicle_profile_metadata.hpp",
            "vehicle_profile_runtime.hpp", "capability_report.json", "artifact_set.json", "vehicle_profile.json",
            "validation_policies.json", "source_report.json"})
        report = json.loads((directory / "source_report.json").read_text())
        self.assertEqual(report["sources"][0]["id"], "demo-a")
        self.assertIn("unmapped_field", [d["code"] for d in report["diagnostics"]])
        header = (directory / "vehicle_profile_metadata.hpp").read_text()
        # user_mapped evidence is the lowest catalog status, never Confirmed.
        self.assertIn("ValidationStatus::Reference", header)
        runtime = (directory / "vehicle_profile_runtime.hpp").read_text()
        self.assertIn("kDispatchTable", runtime)
        self.assertIn("kFreshnessPolicies", runtime)
        profile = json.loads((directory / "vehicle_profile.json").read_text())
        self.assertEqual(profile["profile_id"], "synthetic.demo")
        listed = json.loads((directory / "artifact_set.json").read_text())["outputs"]
        self.assertEqual(set(listed), names - {"artifact_set.json"})

    def test_derived_registry_is_published_and_reloadable(self):
        self.assertEqual(self.run_cli().returncode, 0)
        policies = load_policies((self.current() / "validation_policies.json").read_bytes())
        self.assertIn("synthetic.demo", policies.profile_revisions)
        self.assertEqual(policies.handlers, {})
        self.assertEqual(policies.invalid_value_policies, frozenset({"unavailable"}))

    def test_empty_bindings_publishes_all_features_disabled(self):
        result = self.run_cli(mapping="mapping-empty.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.current() / "capability_report.json").read_text())
        self.assertTrue(report["all_features_disabled"])

    def test_repeat_runs_are_byte_identical_and_path_free(self):
        self.assertEqual(self.run_cli().returncode, 0)
        first = self.tree(self.current())
        other = self.work / "other"
        self.assertEqual(self.run_cli(out=other).returncode, 0)
        self.assertEqual(first, self.tree(self.current(other)))
        for data in first.values():
            self.assertNotIn(str(self.work).encode(), data)
            self.assertNotIn(b"/Users/", data)

    def test_check_passes_then_fails_on_stale_without_mutation(self):
        self.assertEqual(self.run_cli().returncode, 0)
        self.assertEqual(self.run_cli("--check").returncode, 0)
        before = self.tree(self.current())
        index = (self.out / "current.json").read_bytes()
        mapping = self.inputs / "mapping-demo-a.json"
        mapping.write_bytes(mapping.read_bytes() + b"\n")
        self.assertEqual(self.run_cli("--check").returncode, 1)
        self.assertEqual((self.out / "current.json").read_bytes(), index)
        self.assertEqual(self.tree(self.current()), before)

    def test_check_fails_on_missing_and_corrupt_sets(self):
        self.assertEqual(self.run_cli("--check").returncode, 1)
        self.assertFalse(self.out.exists())
        self.assertEqual(self.run_cli().returncode, 0)
        (self.current() / "capability_report.json").write_bytes(b"{}\n")
        self.assertEqual(self.run_cli("--check").returncode, 1)

    def test_mapping_bytes_change_generation(self):
        self.assertEqual(self.run_cli().returncode, 0)
        first = self.current().name
        mapping = self.inputs / "mapping-demo-a.json"
        mapping.write_bytes(mapping.read_bytes() + b"\n")
        other = self.work / "other"
        self.assertEqual(self.run_cli(out=other).returncode, 0)
        self.assertNotEqual(self.current(other).name, first)

    def test_changed_source_bytes_fail_the_pinned_digest(self):
        source = self.inputs / "demo-a.dbc"
        source.write_bytes(source.read_bytes() + b"\n")
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("source_digest_mismatch", failed.stderr)

    def test_every_importer_module_mapping_and_source_enter_identity(self):
        self.assertEqual(self.run_cli().returncode, 0)
        keys = json.loads((self.current() / "artifact_set.json").read_text())["inputs"]
        modules = sorted(p.name for p in (ROOT / "tools/vehicle_profiles").glob("*.py"))
        self.assertTrue(modules)
        for module in modules:
            self.assertIn("code:vehicle_profiles/" + module, keys)
        for key in ("code:import_vehicle_profile.py", "mapping", "source:demo-a",
                    "source-report", "vehicle-profile", "validation-policies"):
            self.assertIn(key, keys)

    def test_importer_code_and_mapping_change_generator_identity(self):
        files = import_vehicle_profile.generator_files()
        base = import_vehicle_profile.generator_identity(b"{}", files)
        victim = self.work / "dbc_import.py"
        victim.write_text("# changed\n")
        changed = dict(files, **{"vehicle_profiles/dbc_import.py": victim})
        self.assertNotEqual(base, import_vehicle_profile.generator_identity(b"{}", changed))
        self.assertNotEqual(base, import_vehicle_profile.generator_identity(b"{ }", files))

    def test_import_limits_enter_artifact_identity_only(self):
        arguments = ["--mapping", str(self.inputs / "mapping-demo-a.json")]
        self.assertEqual(import_vehicle_profile.main(arguments + ["--output-dir", str(self.out)]), 0)

        def identities(root):
            files = self.tree(self.current(root))
            report = json.loads(files["capability_report.json"])
            profile = json.loads(files["vehicle_profile.json"])
            self.assertEqual(profile["artifact_identity"], report["artifact_identity"])
            self.assertEqual(profile["compatibility_identity"], report["compatibility_identity"])
            return report["artifact_identity"], report["compatibility_identity"]

        base = identities(self.out)
        other = self.work / "limits"
        with patch.dict(dbc_import.LIMITS, {"max_records": 65535}):
            self.assertEqual(import_vehicle_profile.main(arguments + ["--output-dir", str(other)]), 0)
        changed = identities(other)
        self.assertNotEqual(base[0], changed[0])
        self.assertEqual(base[1], changed[1])

    def test_fatal_import_preserves_previous_generation_and_prints_codes(self):
        self.assertEqual(self.run_cli().returncode, 0)
        before = self.tree(self.current())
        index = (self.out / "current.json").read_bytes()
        (self.inputs / "demo-a.dbc").write_bytes(b'VERSION "x\nBO_ junk\n')
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("source_digest_mismatch", failed.stderr)
        self.assertNotIn(str(self.work), failed.stderr)
        self.assertEqual((self.out / "current.json").read_bytes(), index)
        self.assertEqual(self.tree(self.current()), before)

    def test_mapping_error_code_is_reported(self):
        mapping = self.inputs / "mapping-demo-a.json"
        mapping.write_text(mapping.read_text().replace("vehicle.engine_rpm", "vehicle.nope"))
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("unknown_target", failed.stderr)
        self.assertFalse(self.out.exists())

    def test_invalid_timeout_prints_stable_freshness_code_and_location(self):
        self.assertEqual(self.run_cli().returncode, 0)
        before = self.tree(self.current())
        index = (self.out / "current.json").read_bytes()
        mapping = self.inputs / "mapping-demo-a.json"
        original = json.loads(mapping.read_text())
        for value in (0, -1, True, 1.5, "10", 4294967296):
            with self.subTest(value=value):
                data = json.loads(json.dumps(original))
                data["bindings"][0]["timeout_ms"] = value
                mapping.write_text(json.dumps(data, indent=2))
                failed = self.run_cli()
                self.assertEqual(failed.returncode, 1)
                self.assertIn("invalid_freshness", failed.stderr)
                self.assertNotIn("mapping_error", failed.stderr)
                text = mapping.read_text().splitlines()
                line = next(i for i, t in enumerate(text, 1) if '"timeout_ms"' in t)
                column = text[line - 1].index(":") + 3  # one-based start of the value
                self.assertIn(f"invalid_freshness at mapping.json:{line}:{column}: "
                              "timeout_ms must be an integer in 1..4294967295", failed.stderr)
                self.assertNotIn(str(self.work), failed.stderr)
                self.assertEqual((self.out / "current.json").read_bytes(), index)
                self.assertEqual(self.tree(self.current()), before)

    def test_file_shape_mapping_error_prints_invalid_mapping(self):
        (self.inputs / "mapping-demo-a.json").write_text("{}")
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("invalid_mapping at mapping.json:1:1:", failed.stderr)
        self.assertNotIn("mapping_error", failed.stderr)

    def test_malformed_mapping_exits_nonzero(self):
        (self.inputs / "mapping-demo-a.json").write_text("{}")
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertFalse(self.out.exists())

    def test_removed_arguments_are_rejected(self):
        for flag in ("--request", "--pack", "--policies"):
            command = [sys.executable, str(TOOL), "--mapping",
                       str(self.inputs / "mapping-demo-a.json"),
                       "--output-dir", str(self.out), flag, "x"]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)

    def test_output_inside_repository_must_be_under_build(self):
        inside = ROOT / "tests" / "not-build-output"
        failed = self.run_cli(out=inside)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("build", failed.stderr)
        self.assertFalse(inside.exists())

    def test_late_failure_after_prior_publish_keeps_previous_generation(self):
        mapping = self.inputs / "mapping-demo-a.json"
        arguments = ["--mapping", str(mapping), "--output-dir", str(self.out)]
        self.assertEqual(import_vehicle_profile.main(arguments), 0)
        before = self.tree(self.current())
        index = (self.out / "current.json").read_bytes()
        mapping.write_bytes(mapping.read_bytes() + b"\n")  # a new generation to publish
        calls = []

        def failing_write(path, data):
            calls.append(path.name)
            if len(calls) == 5:
                raise OSError("injected late failure")
            _write(path, data)

        def faulty_publish(root, outputs):
            return publish(root, outputs, write=failing_write)

        with patch.object(import_vehicle_profile, "publish", faulty_publish):
            self.assertEqual(import_vehicle_profile.main(arguments), 1)
        self.assertGreaterEqual(len(calls), 5)
        self.assertEqual((self.out / "current.json").read_bytes(), index)
        self.assertEqual(self.tree(self.current()), before)
        self.assertEqual([p for p in (self.out / "sets").iterdir()
                          if p.name.startswith(".staging-")], [])


if __name__ == "__main__":
    unittest.main()
