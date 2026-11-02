"""CMake selection resolves imported profiles from the repository root."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"


class VehicleProfileSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repository"
        tools = self.root / "tools"
        modules = tools / "vehicle_profiles"
        modules.mkdir(parents=True)
        for name in ("vehicle_profile.cmake", "import_vehicle_profile.py",
                     "controller_signal_catalog.json"):
            shutil.copy2(ROOT / "tools" / name, tools / name)
        for module in (ROOT / "tools/vehicle_profiles").glob("*.py"):
            shutil.copy2(module, modules / module.name)

        fixtures = self.root / "tests/fixtures/vehicle-profiles/dbc"
        fixtures.mkdir(parents=True)
        for path in FIXTURES.iterdir():
            if path.is_file():
                shutil.copy2(path, fixtures / path.name)

        mapping = json.loads((fixtures / "mapping-demo-a.json").read_text())
        demo_b = (fixtures / "demo-b.dbc").read_bytes()
        mapping["sources"].append({
            "path": "demo-b.dbc",
            "sha256": hashlib.sha256(demo_b).hexdigest(),
            "bus": "body",
            "license": "Apache-2.0; synthetic fixture",
            "encoding": "utf-8",
        })
        (fixtures / "mapping-two-source.json").write_text(json.dumps(mapping) + "\n")

    def project(self, name):
        source = self.root / name
        source.mkdir(parents=True, exist_ok=True)
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(vehicle_profile_selection NONE)
include("{self.root / 'tools/vehicle_profile.cmake'}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "{self.root / 'tools/../tools/controller_signal_catalog.json'}")
vehicle_profile_select()
get_property(selection_dependencies DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
string(REPLACE ";" "\\n" selection_dependencies "${{selection_dependencies}}")
file(WRITE "${{CMAKE_BINARY_DIR}}/dependencies.txt" "${{selection_dependencies}}")
file(WRITE "${{CMAKE_BINARY_DIR}}/selection.txt"
  "native=${{VEHICLE_PROFILE_IS_NATIVE}}\\n"
  "catalog=${{VEHICLE_PROFILE_COMPILER_CATALOG}}\\n"
  "metadata=${{VEHICLE_PROFILE_METADATA_HEADER}}\\n"
  "runtime=${{VEHICLE_PROFILE_RUNTIME_HEADER}}\\n"
  "artifact=${{VEHICLE_PROFILE_ARTIFACT_DIR}}\\n"
  "include=${{VEHICLE_PROFILE_INCLUDE_DIR}}\\n"
  "report=${{VEHICLE_PROFILE_CAPABILITY_REPORT}}\\n"
  "target=${{VEHICLE_PROFILE_ARTIFACT_TARGET}}\\n")
''')
        return source

    def configure(self, source, build, selector=None, cwd=None):
        command = ["cmake", "-S", str(source), "-B", str(build)]
        if selector is not None:
            command.append(f"-DVEHICLE_PROFILE={selector}")
        return subprocess.run(command, cwd=cwd, capture_output=True, text=True)

    @staticmethod
    def selection(build):
        values = {}
        for line in (build / "selection.txt").read_text().splitlines():
            key, value = line.split("=", 1)
            values[key] = value
        return values

    def test_native_default_keeps_the_checked_in_catalog(self):
        source = self.project("host-native")
        build = Path(self.temporary.name) / "build-native"
        result = self.configure(source, build, cwd=self.root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        selected = self.selection(build)
        self.assertEqual(selected["native"], "TRUE")
        self.assertEqual(Path(selected["catalog"]), self.root / "tools/controller_signal_catalog.json")
        self.assertEqual(selected["metadata"], "")
        self.assertEqual(selected["runtime"], "")
        self.assertEqual(selected["artifact"], "")

    def test_host_and_firmware_use_one_generation_for_two_source_and_empty_mappings(self):
        fixtures = "tests/fixtures/vehicle-profiles/dbc/"
        for selector, expected_sources, expected_bindings in (
            (fixtures + "mapping-two-source.json", 2, 1),
            (fixtures + "mapping-empty.json", 1, 0),
        ):
            generations = []
            for name, cwd in (("host", self.root), ("firmware/target", self.root / "firmware")):
                source = self.project(name)
                build = Path(self.temporary.name) / f"build-{name.replace('/', '-')}"
                result = self.configure(source, build, selector, cwd)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                selected = self.selection(build)
                self.assertEqual(selected["native"], "FALSE")
                artifact = Path(selected["artifact"])
                catalog = Path(selected["catalog"])
                metadata = Path(selected["metadata"])
                runtime = Path(selected["runtime"])
                report = Path(selected["report"])
                self.assertEqual(Path(selected["include"]), artifact)
                self.assertTrue(artifact.is_dir())
                self.assertEqual({catalog.parent, metadata.parent, runtime.parent, report.parent},
                                 {artifact})
                self.assertTrue(catalog.is_file())
                self.assertTrue(runtime.is_file())
                self.assertTrue((artifact / "vehicle_profile_metadata.hpp").is_file())
                self.assertTrue(report.is_file())
                self.assertEqual(selected["target"], "vehicle_profile_artifacts")

                built = subprocess.run(["cmake", "--build", str(build),
                                        "--target", selected["target"]],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                profile = json.loads((artifact / "vehicle_profile.json").read_text())
                self.assertEqual(len(profile["sources"]), expected_sources)
                self.assertEqual(len(profile["bindings"]), expected_bindings)
                generations.append(artifact.name)
            self.assertEqual(generations[0], generations[1])

    def test_empty_missing_and_invalid_explicit_selections_fail_configure(self):
        cases = ("", "tests/fixtures/vehicle-profiles/dbc/missing.json")
        for index, selector in enumerate(cases):
            source = self.project(f"invalid-{index}")
            result = self.configure(source, Path(self.temporary.name) / f"build-invalid-{index}",
                                    selector, self.root)
            self.assertNotEqual(result.returncode, 0)

        invalid = self.root / "tests/fixtures/vehicle-profiles/dbc/invalid.json"
        invalid.write_text("{}\n")
        source = self.project("invalid-mapping")
        result = self.configure(
            source, Path(self.temporary.name) / "build-invalid-mapping",
            "tests/fixtures/vehicle-profiles/dbc/invalid.json", self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vehicle profile import failed", result.stdout + result.stderr)

    def test_mapping_source_and_generator_are_configure_dependencies(self):
        selector = "tests/fixtures/vehicle-profiles/dbc/mapping-two-source.json"
        source = self.project("host-regeneration")
        build = Path(self.temporary.name) / "build-regeneration"
        configured = self.configure(source, build, selector, self.root)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        original = Path(self.selection(build)["artifact"])
        dependencies = (build / "dependencies.txt").read_text()
        fixture = self.root / selector
        dbcs = (fixture.parent / "demo-a.dbc", fixture.parent / "demo-b.dbc")
        generator = self.root / "tools/vehicle_profiles/dbc_mapping.py"
        catalog = self.root / "tools/controller_signal_catalog.json"
        catalog_dependencies = [Path(path).resolve() for path in dependencies.splitlines()
                                if Path(path).exists() and Path(path).resolve() == catalog.resolve()]
        self.assertEqual(len(catalog_dependencies), 1)
        for path in (fixture, *dbcs, generator):
            self.assertIn(str(path), dependencies)

        fixture.write_text(fixture.read_text() + "\n")
        rebuilt = subprocess.run(["cmake", "--build", str(build)],
                                 capture_output=True, text=True)
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
        after_mapping = Path(self.selection(build)["artifact"])
        self.assertNotEqual(original, after_mapping)

        generator.write_text(generator.read_text() + "\n")
        rebuilt = subprocess.run(["cmake", "--build", str(build)],
                                 capture_output=True, text=True)
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
        after_generator = Path(self.selection(build)["artifact"])
        self.assertNotEqual(after_mapping, after_generator)

        dbcs[0].write_text(dbcs[0].read_text() + "\n")
        failed = subprocess.run(["cmake", "--build", str(build)],
                                capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("source_digest_mismatch", failed.stdout + failed.stderr)


if __name__ == "__main__":
    unittest.main()
