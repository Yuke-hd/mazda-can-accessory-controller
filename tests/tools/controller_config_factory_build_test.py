"""Build guards for the required ESP-IDF factory configuration embedding."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.tools.compile_controller_config_test import generate_catalog


REPOSITORY = Path(__file__).resolve().parents[2]
COMPONENT = REPOSITORY / "components" / "controller_config"


class FactoryBuildTests(unittest.TestCase):
    cmake = "cmake"
    compiler = "c++"

    def configure(self, directory: Path, contents: str, arguments: list[str] | None = None) -> subprocess.CompletedProcess:
        source = directory / "source"
        source.mkdir(exist_ok=True)
        command = [self.cmake, "-S", str(source), "-B", str(directory / "build"),
                   f"-DCMAKE_CXX_COMPILER={self.compiler}"]
        command.extend(arguments or [])
        (source / "CMakeLists.txt").write_text(contents, encoding="utf-8")
        return subprocess.run(
            command,
            capture_output=True, text=True, check=False,
        )

    def firmware_factory_project(self) -> str:
        firmware = REPOSITORY / "firmware" / "weact-can485-v1.1"
        contents = (firmware / "CMakeLists.txt").read_text(encoding="utf-8")
        contents = contents.split('include($ENV{IDF_PATH}/tools/cmake/project.cmake)', 1)[0]
        contents = contents.replace("${CMAKE_CURRENT_LIST_DIR}", firmware.as_posix())
        return contents + "\nproject(factory_generation_guard NONE)\n"

    def test_firmware_factory_compiler_uses_generated_catalog_at_configure_and_build(self):
        firmware = REPOSITORY / "firmware" / "weact-can485-v1.1"
        # Exercise the real pre-IDF generation rules without requiring the SDK.
        contents = self.firmware_factory_project()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.configure(directory, contents)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = directory / "build" / "factory_default_config.json"
            expected = output.read_bytes()
            self.assertEqual(
                hashlib.sha256(expected).hexdigest(),
                "a8dfaf97e04b0ad0e188ec95e466daad96d2a141a82f4c9b08ba1f499a13b79b",
            )
            self.assertTrue((directory / "build/output_catalog/output_catalog.json").is_file())
            output.unlink()
            result = subprocess.run(
                [self.cmake, "--build", str(directory / "build"),
                 "--target", "controller_config_factory_json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(output.read_bytes(), expected)

    def test_imported_populated_without_timeout_and_empty_profiles_compile_black(self):
        contents = self.firmware_factory_project()
        mappings = REPOSITORY / "tests" / "fixtures" / "vehicle-profiles" / "dbc"
        for mapping_name in ("mapping-demo-a.json", "mapping-empty.json"):
            with self.subTest(mapping=mapping_name), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                mapping = mappings / mapping_name
                result = self.configure(
                    directory, contents, [f"-DVEHICLE_PROFILE={mapping.as_posix()}"]
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                configure_output = result.stdout + result.stderr
                output = directory / "build" / "factory_default_config.json"
                result = subprocess.run(
                    [self.cmake, "--build", str(directory / "build"),
                     "--target", "controller_config_factory_json"],
                    capture_output=True, text=True, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                compiled = json.loads(output.read_text(encoding="utf-8"))
                self.assertEqual(compiled["actions"], [])
                self.assertEqual(compiled["rules"], [])
                self.assertEqual(compiled["outputs"], [])
                board_catalog = json.loads(
                    (directory / "build/output_catalog/output_catalog.json").read_text()
                )
                self.assertTrue(board_catalog["outputs"])
                if mapping_name == "mapping-demo-a.json":
                    self.assertIn("no declared Fresh timeout", configure_output)

    def test_explicit_factory_yaml_is_compiled_strictly_without_pruning(self):
        contents = self.firmware_factory_project()
        mapping = REPOSITORY / "tests" / "fixtures" / "vehicle-profiles" / "dbc" / "mapping-empty.json"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            explicit_yaml = directory / "explicit-default.yaml"
            explicit_yaml.write_text((REPOSITORY / "config/default.yaml").read_text(), encoding="utf-8")
            result = self.configure(
                directory,
                contents,
                [f"-DVEHICLE_PROFILE={mapping.as_posix()}",
                 f"-DCONTROLLER_CONFIG_DEFAULT_YAML={explicit_yaml.as_posix()}"]
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("vehicle.turn_state", result.stdout + result.stderr)

    def test_populated_profile_with_timeout_keeps_rpm_features_fresh(self):
        contents = self.firmware_factory_project()
        fixture_root = REPOSITORY / "tests" / "fixtures" / "vehicle-profiles" / "dbc"
        source_dbc = fixture_root / "demo-a.dbc"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            mapping = json.loads((fixture_root / "mapping-demo-a.json").read_text())
            mapping["sources"][0]["path"] = "demo-a.dbc"
            mapping["sources"][0]["sha256"] = hashlib.sha256(source_dbc.read_bytes()).hexdigest()
            mapping["bindings"][0]["timeout_ms"] = 100
            (directory / "demo-a.dbc").write_bytes(source_dbc.read_bytes())
            mapping_path = directory / "mapping.json"
            mapping_path.write_text(json.dumps(mapping), encoding="utf-8")

            result = self.configure(
                directory, contents, [f"-DVEHICLE_PROFILE={mapping_path.as_posix()}"]
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = directory / "build" / "factory_default_config.json"
            result = subprocess.run(
                [self.cmake, "--build", str(directory / "build"),
                 "--target", "controller_config_factory_json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            compiled = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual([action["name"] for action in compiled["actions"]],
                             ["rpm_fill", "red_zone"])
            self.assertTrue(all(rule["freshness"] == "fresh" for rule in compiled["rules"]))
            self.assertEqual([output["action"] for output in compiled["outputs"]],
                             ["rpm_fill", "red_zone"])

    def test_selected_catalog_and_board_catalog_edits_recompile_factory_json(self):
        for changed_input in ("signal_catalog.json", "output_catalog.json"):
            with self.subTest(input=changed_input), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                source = directory / "source"
                source.mkdir()
                generated_output_catalog = generate_catalog(directory)
                (source / "output_catalog.json").write_bytes(generated_output_catalog.read_bytes())
                (source / "signal_catalog.json").write_bytes(
                    (REPOSITORY / "tools/controller_signal_catalog.json").read_bytes()
                )
                repository = REPOSITORY.as_posix()
                cmake_contents = f"""
cmake_minimum_required(VERSION 3.20)
project(factory_dependency_guard NONE)
include("{repository}/tools/controller_config_factory.cmake")
controller_config_factory_generate(
  YAML "{repository}/config/default.yaml"
  OUTPUT_JSON "${{CMAKE_BINARY_DIR}}/factory_default_config.json"
  OUTPUT_CATALOG "${{CMAKE_SOURCE_DIR}}/output_catalog.json"
  SIGNAL_CATALOG "${{CMAKE_SOURCE_DIR}}/signal_catalog.json")
"""
                result = self.configure(directory, cmake_contents)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                input_path = source / changed_input
                document = json.loads(input_path.read_text(encoding="utf-8"))
                if changed_input == "signal_catalog.json":
                    document["signals"].pop("vehicle.turn_state")
                    expected = "vehicle.turn_state"
                else:
                    document["outputs"] = [entry for entry in document["outputs"]
                                           if entry["type"] != "led_effect"]
                    expected = "led_effect"
                input_path.write_text(json.dumps(document), encoding="utf-8")
                result = subprocess.run(
                    [self.cmake, "--build", str(directory / "build"),
                     "--target", "controller_config_factory_json"],
                    capture_output=True, text=True, check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_component_rejects_unset_and_missing_factory_json(self):
        for selection in ("", 'set(CONTROLLER_CONFIG_FACTORY_JSON "missing.json")'):
            with self.subTest(selection=selection), tempfile.TemporaryDirectory() as temporary:
                result = self.configure(Path(temporary), f"""
cmake_minimum_required(VERSION 3.20)
project(factory_component_guard NONE)
function(idf_component_register)
  message(FATAL_ERROR "Registration reached without a factory JSON document")
endfunction()
{selection}
include("{COMPONENT.as_posix()}/CMakeLists.txt")
""")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("CONTROLLER_CONFIG_FACTORY_JSON must name an existing", result.stderr)

    def test_idf_dependency_discovery_does_not_require_project_cache_variables(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.configure(Path(temporary), f"""
cmake_minimum_required(VERSION 3.20)
project(factory_dependency_discovery NONE)
set(CMAKE_BUILD_EARLY_EXPANSION TRUE)
macro(idf_component_register)
  return()
endmacro()
include("{COMPONENT.as_posix()}/CMakeLists.txt")
""")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_esp_source_requires_embedding_but_host_source_compiles(self):
        for esp_platform in (False, True):
            with self.subTest(esp_platform=esp_platform), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                definition = "target_compile_definitions(factory PRIVATE ESP_PLATFORM=1)" if esp_platform else ""
                result = self.configure(directory, f"""
cmake_minimum_required(VERSION 3.20)
project(factory_source_guard LANGUAGES CXX)
add_library(factory STATIC "{COMPONENT.as_posix()}/src/factory_default.cpp")
target_include_directories(factory PRIVATE "{COMPONENT.as_posix()}/include")
target_compile_features(factory PRIVATE cxx_std_17)
{definition}
""")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(
                    [self.cmake, "--build", str(directory / "build")],
                    capture_output=True, text=True, check=False,
                )
                if esp_platform:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("ESP-IDF requires the embedded factory configuration",
                                  result.stdout + result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--compiler", default="c++")
    options, unittest_args = parser.parse_known_args()
    FactoryBuildTests.cmake = options.cmake
    FactoryBuildTests.compiler = options.compiler
    unittest.main(argv=[__file__, *unittest_args])
