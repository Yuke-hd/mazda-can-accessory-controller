"""Build guards for the required ESP-IDF factory configuration embedding."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
COMPONENT = REPOSITORY / "components" / "controller_config"


class FactoryBuildTests(unittest.TestCase):
    cmake = "cmake"
    compiler = "c++"

    def configure(self, directory: Path, contents: str) -> subprocess.CompletedProcess:
        source = directory / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(contents, encoding="utf-8")
        return subprocess.run(
            [self.cmake, "-S", str(source), "-B", str(directory / "build"),
             f"-DCMAKE_CXX_COMPILER={self.compiler}"],
            capture_output=True, text=True, check=False,
        )

    def test_firmware_factory_compiler_uses_generated_catalog_at_configure_and_build(self):
        firmware = REPOSITORY / "firmware" / "weact-can485-v1.1"
        # Exercise the real pre-IDF generation rules without requiring the SDK.
        contents = (firmware / "CMakeLists.txt").read_text(encoding="utf-8")
        contents = contents.split('include($ENV{IDF_PATH}/tools/cmake/project.cmake)', 1)[0]
        contents = contents.replace("${CMAKE_CURRENT_LIST_DIR}", firmware.as_posix())
        contents += "\nproject(factory_generation_guard NONE)\n"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.configure(directory, contents)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = directory / "build" / "factory_default_config.json"
            expected = output.read_bytes()
            self.assertTrue((directory / "build/output_catalog/output_catalog.json").is_file())
            output.unlink()
            result = subprocess.run(
                [self.cmake, "--build", str(directory / "build"),
                 "--target", "controller_config_factory_json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(output.read_bytes(), expected)

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
