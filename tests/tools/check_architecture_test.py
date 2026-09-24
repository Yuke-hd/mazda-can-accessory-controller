"""Regression tests for the architecture boundary checker."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from typing import Tuple


TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import check_architecture  # noqa: E402


class ArchitectureCheckerRegressionTests(unittest.TestCase):
    def make_vehicle_signals_fixture(self, root: Path) -> Tuple[Path, Path]:
        core_root = root / "core"
        core = core_root / "components/vehicle_core"
        include = core / "include/vehicle_core"
        include.mkdir(parents=True)
        (include / "telemetry_contracts.hpp").write_text(
            "#pragma once\n"
            "namespace vehicle_core {\n"
            "template <typename T> struct Reading {};\n"
            "template <typename T> struct Notification {};\n"
            "enum class Availability { Unknown, Available, Unavailable };\n"
            "enum class ValidationStatus { Reference, Observed, Confirmed };\n"
            "}\n",
            encoding="utf-8",
        )
        (core / "CMakeLists.txt").write_text(
            "add_library(vehicle_core INTERFACE)\n"
            "target_include_directories(vehicle_core INTERFACE include)\n",
            encoding="utf-8",
        )
        shutil.copytree(
            Path(__file__).resolve().parents[2] / "lib/vehicle_signals",
            root / "lib/vehicle_signals",
        )
        return core_root, root / "lib/vehicle_signals"

    def check_vehicle_signals(self, root: Path, core_root: Path) -> None:
        work_dir = root / "work"
        work_dir.mkdir()
        check_architecture._check_vehicle_signals(
            root,
            "cmake",
            ("c++",),
            work_dir,
            core_root,
        )

    def test_vehicle_signals_public_headers_use_only_value_core_contracts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            core_root, _ = self.make_vehicle_signals_fixture(root)
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.check_vehicle_signals(root, core_root)

    def test_vehicle_signals_rejects_mazda_target_and_include_exports(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            core_root, signals = self.make_vehicle_signals_fixture(root)
            (root / "lib/mazda/include/mazda").mkdir(parents=True)
            (root / "lib/mazda/include/mazda/definitions.hpp").write_text(
                "#pragma once\nnamespace mazda { struct Definition {}; }\n",
                encoding="utf-8",
            )
            cmake = signals / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "\ntarget_include_directories(vehicle_signals INTERFACE "
                + f"{root / 'lib/mazda/include'})\n"
                + "target_link_libraries(vehicle_signals INTERFACE mazda)\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.check_vehicle_signals(root, core_root)
        detail = str(raised.exception).lower()
        self.assertIn("forbidden", detail)
        self.assertTrue("mazda" in detail or "include directory" in detail)

    def test_vehicle_signals_rejects_a_mazda_link_target(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            core_root, signals = self.make_vehicle_signals_fixture(root)
            cmake = signals / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "\ntarget_link_libraries(vehicle_signals INTERFACE mazda)\n",
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.check_vehicle_signals(root, core_root)
        self.assertIn("links forbidden target", str(raised.exception))

    def test_vehicle_signals_rejects_non_value_core_header_transitively(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-signals-fixture-") as directory:
            root = Path(directory)
            core_root, signals = self.make_vehicle_signals_fixture(root)
            core_headers = core_root / "components/vehicle_core/include/vehicle_core"
            (core_headers / "frame.hpp").write_text(
                "#pragma once\nnamespace vehicle_core { struct RawCanFrame {}; }\n",
                encoding="utf-8",
            )
            catalog = signals / "include/vehicle_signals/catalog.hpp"
            catalog.write_text(
                '#include "vehicle_core/frame.hpp"\n'
                + catalog.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.check_vehicle_signals(root, core_root)
        self.assertIn("non-value vehicle_core dependency", str(raised.exception))

    def test_vehicle_core_target_private_mazda_dependency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-core-fixture-") as directory:
            root = Path(directory)
            core = root / "third_party/esp32-vehicle-can-core/components/vehicle_core"
            include = core / "include/vehicle_core"
            source_dir = core / "src"
            include.mkdir(parents=True)
            source_dir.mkdir()
            (include / "vehicle_core.hpp").write_text(
                "#pragma once\n"
                "namespace vehicle_core {\n"
                "struct RawCanFrame { bool is_valid() const { return true; } };\n"
                "inline bool library_is_available() { return true; }\n"
                "}\n",
                encoding="utf-8",
            )
            (include / "reading.hpp").write_text(
                "#pragma once\n"
                "namespace vehicle_core { template <typename T> struct Reading { T value{}; }; }\n",
                encoding="utf-8",
            )
            (include / "notification.hpp").write_text(
                "#pragma once\n#include \"vehicle_core/reading.hpp\"\n"
                "namespace vehicle_core { template <typename T> struct Notification { Reading<T> current{}; }; }\n",
                encoding="utf-8",
            )
            (include / "telemetry_contracts.hpp").write_text(
                "#pragma once\n#include \"vehicle_core/reading.hpp\"\n"
                "#include \"vehicle_core/notification.hpp\"\n",
                encoding="utf-8",
            )
            (root / "lib/mazda/include/mazda").mkdir(parents=True)
            (root / "lib/mazda/include/mazda/definitions.hpp").write_text(
                "#pragma once\nnamespace mazda { struct Definition {}; }\n",
                encoding="utf-8",
            )
            source = source_dir / "vehicle_core.cpp"
            source.write_text(
                '#include "mazda/definitions.hpp"\n'
                '#include "vehicle_core/vehicle_core.hpp"\n',
                encoding="utf-8",
            )
            cmake = core / "CMakeLists.txt"
            cmake.write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(vehicle_core_fixture LANGUAGES CXX)\n"
                "add_library(vehicle_core STATIC src/vehicle_core.cpp)\n"
                "target_include_directories(vehicle_core PUBLIC include PRIVATE "
                f"{root / 'lib/mazda/include'})\n",
                encoding="utf-8",
            )

            work_dir = root / "work"
            work_dir.mkdir()
            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    check_architecture._check_core_only(
                        root,
                        "cmake",
                        ("c++",),
                        work_dir,
                    )

            detail = str(raised.exception)
            self.assertIn("vehicle_core target", detail)
            self.assertIn("lib/mazda", detail)

    def test_active_cmake_and_yaml_build_files_reject_retired_capture_marker(self) -> None:
        with tempfile.TemporaryDirectory(prefix="architecture-capture-fixture-") as directory:
            root = Path(directory)
            files = (
                root / "CMakeLists.txt",
                root / "components/example/CMakeLists.txt",
                root / "firmware/example/CMakeLists.txt",
                root / "components/example/idf_component.yaml",
            )
            marker = "raw_" + "capture"
            for path in files:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"set(RETIRED_DEPENDENCY {marker})\n", encoding="utf-8")

            with self.assertRaises(check_architecture.ArchitectureFailure) as raised:
                check_architecture._check_capture_removal(root)

            detail = str(raised.exception)
            for path in files:
                self.assertIn(path.relative_to(root).as_posix(), detail)


if __name__ == "__main__":
    unittest.main()
