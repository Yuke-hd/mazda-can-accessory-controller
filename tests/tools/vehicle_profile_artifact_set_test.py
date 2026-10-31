"""Whole-generation publication and build dependency regression tests."""

import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from vehicle_profiles.artifact_set import check, contents, publish
from vehicle_profiles.projections import ProfileProjections


class ArtifactPublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "output"
        self.old = contents(ProfileProjections(b"catalog-old", b"header-old", b"report-old", b"runtime-old"), {"source:a": "1"})
        self.new = contents(ProfileProjections(b"catalog-new", b"header-new", b"report-new", b"runtime-new"), {"source:a": "2"})

    def tearDown(self):
        self.temporary.cleanup()

    def test_repeat_generation_and_nonmutating_missing_stale_check(self):
        self.assertFalse(check(self.root, self.old))
        self.assertFalse(self.root.exists())
        directory = publish(self.root, self.old)
        snapshot = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.root.rglob("*") if p.is_file()}
        self.assertTrue(check(self.root, self.old))
        self.assertFalse(check(self.root, self.new))
        self.assertEqual(snapshot, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in snapshot})
        self.assertEqual(directory, publish(self.root, self.old))
        self.assertTrue(check(self.root, self.old))
        self.assertEqual(snapshot, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in snapshot})

    def test_late_final_output_failure_preserves_previous_generation_and_readers(self):
        directory = publish(self.root, self.old)
        index = (self.root / "current.json").read_bytes()
        written = []
        def fail_last(path, data):
            written.append(path.name)
            if path.name == "artifact_set.json":
                raise OSError("injected final output failure")
            path.write_bytes(data)
        with self.assertRaisesRegex(OSError, "injected"):
            publish(self.root, self.new, write=fail_last)
        self.assertEqual(len(written), 5)
        self.assertEqual((self.root / "current.json").read_bytes(), index)
        self.assertTrue(check(self.root, self.old))
        for name, data in self.old.items():
            self.assertEqual((directory / name).read_bytes(), data)
        self.assertEqual(list((self.root / "sets").glob(".staging-*")), [])
        new_directory = publish(self.root, self.new)
        self.assertNotEqual(directory, new_directory)
        self.assertTrue(check(self.root, self.new))
        self.assertTrue(directory.exists())

    def test_late_runtime_header_failure_preserves_previous_generation(self):
        directory = publish(self.root, self.old)
        index = (self.root / "current.json").read_bytes()
        written = []

        def fail_runtime_header(path, data):
            written.append(path.name)
            if path.name == "vehicle_profile_runtime.hpp":
                raise OSError("injected runtime header failure")
            path.write_bytes(data)

        with self.assertRaisesRegex(OSError, "runtime header"):
            publish(self.root, self.new, write=fail_runtime_header)
        self.assertEqual(written, [
            "controller_signal_catalog.json",
            "vehicle_profile_metadata.hpp",
            "vehicle_profile_runtime.hpp",
        ])
        self.assertEqual((self.root / "current.json").read_bytes(), index)
        self.assertTrue(check(self.root, self.old))
        self.assertEqual(self.old, {p.name: p.read_bytes() for p in directory.iterdir()})
        self.assertEqual(list((self.root / "sets").glob(".staging-*")), [])

    def test_corrupt_existing_generation_is_rejected(self):
        directory = publish(self.root, self.old)
        (directory / "capability_report.json").write_bytes(b"corrupt")
        self.assertFalse(check(self.root, self.old))
        with self.assertRaises(OSError):
            publish(self.root, self.old)

    def test_symlinked_sets_rejected_without_external_mutation(self):
        directory = publish(self.root, self.old)
        index = (self.root / "current.json").read_bytes()
        outside = self.root.parent / "outside"
        (self.root / "sets").rename(outside)
        (self.root / "sets").symlink_to(outside, target_is_directory=True)
        snapshot = {p: p.read_bytes() for p in outside.rglob("*") if p.is_file()}
        self.assertFalse(check(self.root, self.old))
        for outputs in (self.old, self.new):
            with self.assertRaisesRegex(OSError, "symlink"):
                publish(self.root, outputs)
        self.assertEqual((self.root / "current.json").read_bytes(), index)
        self.assertEqual(snapshot, {p: p.read_bytes() for p in outside.rglob("*") if p.is_file()})
        self.assertEqual(set(outside.iterdir()), {outside / directory.name})

    def test_symlinked_generation_is_rejected_without_external_mutation(self):
        directory = publish(self.root, self.old)
        index = (self.root / "current.json").read_bytes()
        outside = self.root.parent / "outside-generation"
        directory.rename(outside)
        directory.symlink_to(outside, target_is_directory=True)
        self.assertFalse(check(self.root, self.old))
        with self.assertRaisesRegex(OSError, "existing generation is corrupt"):
            publish(self.root, self.old)
        self.assertEqual((self.root / "current.json").read_bytes(), index)
        self.assertEqual(self.old, {p.name: p.read_bytes() for p in outside.iterdir()})
        self.assertEqual(list((self.root / "sets").glob(".staging-*")), [])

    def test_valid_concurrent_same_generation_is_reused(self):
        old_directory = publish(self.root, self.old)
        def concurrent_publication(staging, destination):
            destination.mkdir()
            for name, data in self.new.items():
                (destination / name).write_bytes(data)
            raise FileExistsError("another writer published the same generation")
        with patch("vehicle_profiles.artifact_set.os.rename", side_effect=concurrent_publication):
            directory = publish(self.root, self.new)
        self.assertTrue(check(self.root, self.new))
        self.assertEqual(self.new, {p.name: p.read_bytes() for p in directory.iterdir()})
        self.assertEqual(self.old, {p.name: p.read_bytes() for p in old_directory.iterdir()})
        self.assertEqual(list((self.root / "sets").glob(".staging-*")), [])

    def test_empty_existing_generation_is_rejected_preserving_current(self):
        damaged = publish(self.root, self.new)
        directory = publish(self.root, self.old)
        index = (self.root / "current.json").read_bytes()
        for path in damaged.iterdir():
            path.unlink()
        with self.assertRaisesRegex(OSError, "existing generation is corrupt"):
            publish(self.root, self.new)
        self.assertEqual(list(damaged.iterdir()), [])
        self.assertEqual((self.root / "current.json").read_bytes(), index)
        self.assertTrue(check(self.root, self.old))
        self.assertEqual(self.old, {p.name: p.read_bytes() for p in directory.iterdir()})
        self.assertEqual(list((self.root / "sets").glob(".staging-*")), [])


class ArtifactCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "profile.json"
        profile = json.loads((ROOT / "tests/fixtures/vehicle-profiles/synthetic-river.json").read_text())
        profile["bindings"] = [binding for binding in profile["bindings"] if binding["kind"] == "dbc"]
        self.source_args = []
        for source in profile["sources"]:
            path = self.root / (source["id"] + ".dbc")
            path.write_bytes(b"independently authored synthetic source " + source["id"].encode())
            source["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            self.source_args += ["--source", source["id"] + "=" + str(path)]
        self.manifest.write_text(json.dumps(profile))
        self.policy = self.root / "policies.json"
        self.policy.write_text(json.dumps({"version": 1,
            "profile_revisions": [["synthetic.river", "river.v1", "synthetic.v1"]],
            "handlers": [["synthetic.ready", "v1", ["synthetic.river"], "boolean", "", 0, 0]],
            "evidence": [["synthetic/river-review", "Reference", "source_only"]],
            "units": ["", "km/h"], "unit_conversions": [["km/h", "km/h", "1", "0"]],
            "invalid_value_policies": ["unavailable"], "freshness_policies": []}))
        self.extra = self.root / "generator-input.txt"
        self.extra.write_text("reviewed generator data v1")
        self.output = self.root / "output"
        self.command = [sys.executable, str(ROOT / "tools/generate_vehicle_profile.py"),
                        "--manifest", str(self.manifest), "--policies", str(self.policy),
                        "--generator-revision", "test.v1", "--output-dir", str(self.output),
                        "--generator-input", "data=" + str(self.extra)] + self.source_args

    def tearDown(self):
        self.temporary.cleanup()

    def run_command(self, *args):
        return subprocess.run(self.command + list(args), capture_output=True, text=True)

    def test_real_cli_determinism_sources_generator_inputs_and_nonmutation(self):
        self.assertEqual(self.run_command("--check").returncode, 1)
        self.assertFalse(self.output.exists())
        first = self.run_command("--print-directory")
        self.assertEqual(first.returncode, 0, first.stderr)
        directory = Path(first.stdout.strip())
        snapshot = {p: p.read_bytes() for p in directory.iterdir()}
        self.assertEqual(first.stdout, self.run_command("--print-directory").stdout)
        self.assertEqual(self.run_command("--check").returncode, 0)
        profile = json.loads(self.manifest.read_text())
        profile["sources"].reverse()
        profile["bindings"].reverse()
        self.manifest.write_text(json.dumps(profile, sort_keys=True, indent=4))
        registry = json.loads(self.policy.read_text())
        registry["units"].reverse()
        self.policy.write_text(json.dumps(registry, sort_keys=True, indent=4))
        self.assertEqual(self.run_command("--check").returncode, 0)
        self.assertEqual(first.stdout, self.run_command("--print-directory").stdout)
        self.extra.write_text("reviewed generator data v2")
        index = (self.output / "current.json").read_bytes()
        self.assertEqual(self.run_command("--check").returncode, 1)
        self.assertEqual((self.output / "current.json").read_bytes(), index)
        second = self.run_command("--print-directory")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertNotEqual(first.stdout, second.stdout)
        self.assertEqual(snapshot, {p: p.read_bytes() for p in snapshot})
        for data in snapshot.values():
            self.assertNotIn(str(self.root).encode(), data)
        source = Path(self.source_args[1].split("=", 1)[1])
        source.write_text("changed without reviewed digest")
        failed = self.run_command()
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("source digest mismatch", failed.stderr)

    def test_policy_and_source_inputs_are_strict_and_fail_before_publication(self):
        self.command += ["--source", self.source_args[1]]
        self.assertIn("unique", self.run_command().stderr)
        self.assertFalse(self.output.exists())
        self.command = self.command[:-2]
        self.policy.write_text(self.policy.read_text().replace('"version": 1', '"version": true'))
        self.assertIn("policy registry", self.run_command().stderr)
        self.assertFalse(self.output.exists())

    def test_cmake_configure_build_regeneration_and_explicit_check(self):
        source = self.root / "cmake-source"
        source.mkdir()
        assignments = "\n".join('  "' + self.source_args[i].replace("\\", "/") + '"'
                                  for i in range(1, len(self.source_args), 2))
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(profile_bootstrap NONE)
include("{ROOT / 'tools/vehicle_profile.cmake'}")
vehicle_profile_generate(MANIFEST "{self.manifest}" POLICIES "{self.policy}"
 GENERATOR_REVISION test.v1 SOURCE_FILES
{assignments}
 GENERATOR_INPUTS "data={self.extra}")
file(WRITE "${{CMAKE_BINARY_DIR}}/selected.txt" "${{VEHICLE_PROFILE_ARTIFACT_DIR}}")
file(WRITE "${{CMAKE_BINARY_DIR}}/runtime-header.txt" "${{VEHICLE_PROFILE_RUNTIME_HEADER}}")
''')
        build = self.root / "build"
        def run(*args):
            result = subprocess.run(["cmake", *args], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run("-S", str(source), "-B", str(build), "-DCMAKE_SYSTEM_NAME=Generic")
        selected = (build / "selected.txt").read_text()
        runtime_header = Path((build / "runtime-header.txt").read_text())
        self.assertEqual(runtime_header, Path(selected) / "vehicle_profile_runtime.hpp")
        self.assertTrue(runtime_header.is_file())
        check_command = self.command.copy()
        check_command[check_command.index("--output-dir") + 1] = str(build / "vehicle-profile")
        checked = subprocess.run(check_command + ["--check"], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)

        selected_directory = Path((build / "selected.txt").read_text())
        index = (build / "vehicle-profile/current.json").read_bytes()
        (selected_directory / "vehicle_profile_metadata.hpp").unlink()
        failed = subprocess.run(["cmake", "--build", str(build), "--target", "vehicle_profile_artifacts"],
                                capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("existing generation is corrupt", failed.stdout + failed.stderr)
        self.assertEqual((build / "vehicle-profile/current.json").read_bytes(), index)
        self.assertFalse((selected_directory / "vehicle_profile_metadata.hpp").exists())
        self.assertTrue((selected_directory / "vehicle_profile_runtime.hpp").is_file())
        for path in selected_directory.iterdir():
            path.unlink()
        failed = subprocess.run(["cmake", "--build", str(build), "--target", "vehicle_profile_artifacts"],
                                capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("existing generation is corrupt", failed.stdout + failed.stderr)
        self.assertEqual((build / "vehicle-profile/current.json").read_bytes(), index)
        self.assertEqual(list(selected_directory.iterdir()), [])
        self.assertEqual(list((build / "vehicle-profile/sets").glob(".staging-*")), [])
        recovered = self.run_command("--print-directory")
        self.assertEqual(recovered.returncode, 0, recovered.stderr)
        self.assertTrue((Path(recovered.stdout.strip()) / "vehicle_profile_metadata.hpp").is_file())
        self.assertTrue((Path(recovered.stdout.strip()) / "vehicle_profile_runtime.hpp").is_file())
        self.extra.write_text("changed declared generator input")
        run("--build", str(build), "--target", "vehicle_profile_artifacts")
        self.assertNotEqual(selected, (build / "selected.txt").read_text())
        self.assertTrue(Path(selected).is_dir())
        checked = subprocess.run(check_command + ["--check"], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_cmake_rejects_symlinked_sets_before_generation(self):
        source = self.root / "sets-source"
        source.mkdir()
        build = self.root / "sets-build"
        output = build / "vehicle-profile"
        output.mkdir(parents=True)
        outside = self.root / "outside-sets"
        outside.mkdir()
        (output / "sets").symlink_to(outside, target_is_directory=True)
        assignments = "\n".join('  "' + self.source_args[i] + '"'
                                  for i in range(1, len(self.source_args), 2))
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(profile_sets_escape NONE)
include("{ROOT / 'tools/vehicle_profile.cmake'}")
vehicle_profile_generate(MANIFEST "{self.manifest}" POLICIES "{self.policy}"
 GENERATOR_REVISION test.v1 SOURCE_FILES
{assignments})
''')
        failed = subprocess.run(["cmake", "-S", str(source), "-B", str(build)], capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("symlink", failed.stdout + failed.stderr)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((output / "current.json").exists())

    def test_cmake_rejects_symlink_output_escape_before_generation(self):
        source = self.root / "symlink-source"
        source.mkdir()
        build = self.root / "symlink-build"
        build.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        (build / "linked").symlink_to(outside, target_is_directory=True)
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(profile_escape NONE)
include("{ROOT / 'tools/vehicle_profile.cmake'}")
vehicle_profile_generate(MANIFEST "{self.manifest}" POLICIES "{self.policy}"
 GENERATOR_REVISION test.v1 OUTPUT_DIR "${{CMAKE_BINARY_DIR}}/linked/artifacts")
''')
        failed = subprocess.run(["cmake", "-S", str(source), "-B", str(build)], capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("must resolve inside the build directory", failed.stdout + failed.stderr)
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
