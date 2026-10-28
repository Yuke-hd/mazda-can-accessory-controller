"""End-to-end tests for the DBC import command and artifact-set publication."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
TOOL = ROOT / "tools/import_vehicle_profile.py"


class ImportCommandTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.inputs = self.work / "inputs"
        shutil.copytree(FIXTURES, self.inputs)
        self.out = self.work / "out"

    def run_cli(self, *extra, out=None):
        command = [sys.executable, str(TOOL),
                   "--request", str(self.inputs / "request-demo-a.json"),
                   "--pack", str(self.inputs / "synthetic-pack.json"),
                   "--policies", str(self.inputs / "policies.json"),
                   "--output-dir", str(out or self.out), *extra]
        return subprocess.run(command, capture_output=True, text=True)

    def current(self, out=None):
        root = out or self.out
        index = json.loads((root / "current.json").read_text())
        return root / index["directory"]

    def tree(self, directory):
        return {p.name: p.read_bytes() for p in sorted(directory.iterdir())}

    def test_publishes_complete_set_with_source_report(self):
        result = self.run_cli("--print-directory")
        self.assertEqual(result.returncode, 0, result.stderr)
        directory = self.current()
        self.assertEqual(result.stdout.strip(), str(directory.resolve()))
        names = set(self.tree(directory))
        self.assertTrue({"controller_signal_catalog.json", "vehicle_profile_metadata.hpp",
                         "capability_report.json", "artifact_set.json",
                         "source_report.json"} <= names)
        report = json.loads((directory / "source_report.json").read_text())
        self.assertEqual(report["sources"][0]["id"], "demo-a")
        self.assertIn("unmapped_field", [d["code"] for d in report["diagnostics"]])

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
        source = self.inputs / "demo-a.dbc"
        source.write_bytes(source.read_bytes() + b"\n")
        self.assertEqual(self.run_cli("--check").returncode, 1)
        self.assertEqual((self.out / "current.json").read_bytes(), index)
        self.assertEqual(self.tree(self.current()), before)

    def test_check_fails_on_missing_and_corrupt_sets(self):
        self.assertEqual(self.run_cli("--check").returncode, 1)
        self.assertFalse(self.out.exists())
        self.assertEqual(self.run_cli().returncode, 0)
        (self.current() / "capability_report.json").write_bytes(b"{}\n")
        self.assertEqual(self.run_cli("--check").returncode, 1)

    def test_changing_source_pack_or_request_changes_generation(self):
        self.assertEqual(self.run_cli().returncode, 0)
        first = self.current().name
        for name in ("demo-a.dbc", "synthetic-pack.json", "request-demo-a.json"):
            path = self.inputs / name
            original = path.read_bytes()
            path.write_bytes(original + b"\n")
            out = self.work / ("out-" + name)
            result = self.run_cli(out=out)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotEqual(self.current(out).name, first, name)
            path.write_bytes(original)

    def test_importer_code_enters_the_generation_identity(self):
        self.assertEqual(self.run_cli().returncode, 0)
        keys = json.loads((self.current() / "artifact_set.json").read_text())["inputs"]
        for key in ("code:vehicle_profiles/dbc_import.py", "code:vehicle_profiles/recognition.py",
                    "code:import_vehicle_profile.py", "request", "pack", "policies",
                    "source:demo-a"):
            self.assertIn(key, keys)

    def test_fatal_import_preserves_previous_generation_and_prints_codes(self):
        self.assertEqual(self.run_cli().returncode, 0)
        before = self.tree(self.current())
        index = (self.out / "current.json").read_bytes()
        (self.inputs / "demo-a.dbc").write_bytes(b'VERSION "x\nBO_ junk\n')
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("dbc_syntax_error", failed.stderr)
        self.assertNotIn(str(self.work), failed.stderr)
        self.assertEqual((self.out / "current.json").read_bytes(), index)
        self.assertEqual(self.tree(self.current()), before)

    def test_fatal_import_on_fresh_root_publishes_nothing(self):
        (self.inputs / "demo-a.dbc").write_bytes(b"\xff\xfe")
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertIn("source_decode", failed.stderr)
        self.assertFalse(self.out.exists())

    def test_configuration_error_exits_nonzero(self):
        (self.inputs / "request-demo-a.json").write_text("{}")
        failed = self.run_cli()
        self.assertEqual(failed.returncode, 1)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
