#!/usr/bin/env python3
"""Exercise the public-header checker against temporary fixture mutations."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from typing import Optional


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/check_public_headers.py"
FIXTURE = Path(__file__).resolve().parent / "fixtures/clean"
CHECKER_COMPILER: Optional[str] = None
CHECKER_CMAKE = "cmake"

sys.path.insert(0, str(ROOT))
from tools.check_public_headers import (
    _compile_database_command,
    _include_directory_arguments,
    check_public_headers,
)


def run_checker(root: Path) -> subprocess.CompletedProcess:
    command = [sys.executable, str(CHECKER), "--root", str(root), "--cmake", CHECKER_CMAKE]
    if CHECKER_COMPILER is not None:
        command.extend(("--compiler", CHECKER_COMPILER))
    return subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )


class PublicHeaderCheckerTests(unittest.TestCase):
    def copy_fixture(self, temporary: Path) -> Path:
        destination = temporary / "fixture"
        shutil.copytree(FIXTURE, destination)
        return destination

    def test_clean_fixture_and_access_probes_pass(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            result = run_checker(self.copy_fixture(Path(directory)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Header boundary check passed", result.stdout)
        self.assertIn("normal access failed as expected", result.stdout)
        self.assertIn("authorized access succeeded (internal include path)", result.stdout)
        self.assertIn("OK   mazda/signal_provider.hpp (generic signal provider)", result.stdout)
        self.assertIn("OK   vehicle_signals/signal_contracts.hpp", result.stdout)
        self.assertIn("OK   vehicle_signals/signal_catalog.hpp", result.stdout)
        self.assertIn("OK   vehicle_signals/signal_provider.hpp", result.stdout)
        self.assertIn("OK   action_engine/engine.hpp (generic action engine)", result.stdout)
        self.assertIn("OK   action_engine/action.hpp (generic action engine)", result.stdout)

    def test_provider_reaching_telemetry_service_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            # A public location, so only the provider-specific rule applies.
            include = root / "components/vehicle_telemetry/include/mazda"
            (include / "vehicle_telemetry_service.hpp").write_text(
                "#pragma once\nnamespace mazda::internal { class VehicleTelemetryService; }\n",
                encoding="utf-8",
            )
            provider = include / "signal_provider.hpp"
            provider.write_text(
                '#include "mazda/vehicle_telemetry_service.hpp"\n'
                + provider.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL mazda/signal_provider.hpp (generic signal provider)", output)
        self.assertIn("provider service/state/publication/catalog", output)
        self.assertIn("vehicle_telemetry_service.hpp", output)
        self.assertIn("CMake provider consumer has a forbidden dependency", output)
        self.assertNotIn("CMake facade consumer has a forbidden dependency", output)

    def test_provider_rules_do_not_apply_to_the_typed_facade(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            provider = root / "components/vehicle_telemetry/include/mazda/signal_provider.hpp"
            provider.write_text(
                '#include "mazda/types.hpp"\n' + provider.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL mazda/signal_provider.hpp (generic signal provider)", output)
        self.assertIn("lib/mazda/include/mazda/types.hpp", output)
        # The facade reaches the same Mazda value types legitimately.
        self.assertIn("OK   mazda/facade_contracts.hpp (facade contracts)", output)
        self.assertIn("OK   mazda/vehicle_telemetry.hpp (vehicle telemetry facade)", output)

    def test_vehicle_signals_header_reaching_mazda_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            contracts = root / "lib/vehicle_signals/include/vehicle_signals/signal_contracts.hpp"
            contracts.write_text(
                '#include "mazda/types.hpp"\n' + contracts.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL vehicle_signals/signal_contracts.hpp", output)
        self.assertIn("FAIL vehicle_signals/signal_catalog.hpp", output)
        self.assertIn("Mazda dependency: lib/mazda/include/mazda/types.hpp", output)

    def test_vehicle_signals_header_reaching_mutable_core_signal_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            (root / "lib/vehicle_core/include/vehicle_core/signal.hpp").write_text(
                "#pragma once\nnamespace vehicle_core { template <typename T> class Signal {}; }\n",
                encoding="utf-8",
            )
            catalog = root / "lib/vehicle_signals/include/vehicle_signals/signal_catalog.hpp"
            catalog.write_text(
                '#include "vehicle_core/signal.hpp"\n' + catalog.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL vehicle_signals/signal_catalog.hpp", output)
        self.assertIn("mutable signal/state: lib/vehicle_core/include/vehicle_core/signal.hpp", output)

    def test_action_engine_header_reaching_mazda_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            # A public Mazda header: only the engine-specific rule applies.
            rules = root / "lib/action_engine/include/action_engine/rules.hpp"
            rules.write_text(
                '#include "mazda/signal_provider.hpp"\n' + rules.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL action_engine/rules.hpp (generic action engine)", output)
        self.assertIn("FAIL action_engine/engine.hpp (generic action engine)", output)
        self.assertIn(
            "Mazda dependency: components/vehicle_telemetry/include/mazda/signal_provider.hpp",
            output,
        )
        self.assertIn("OK   action_engine/action.hpp (generic action engine)", output)
        self.assertIn("OK   mazda/signal_provider.hpp (generic signal provider)", output)

    def test_action_engine_header_reaching_an_output_adapter_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            adapter = root / "lib/vehicle_signals/include/local_argb/sink.hpp"
            adapter.parent.mkdir(parents=True)
            adapter.write_text("#pragma once\nnamespace local_argb { class Sink; }\n", encoding="utf-8")
            action = root / "lib/action_engine/include/action_engine/action.hpp"
            action.write_text(
                '#include "local_argb/sink.hpp"\n' + action.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("FAIL action_engine/action.hpp (generic action engine)", output)
        self.assertIn("output adapter dependency", output)
        self.assertIn("OK   vehicle_signals/signal_provider.hpp", output)

    def test_missing_internal_include_is_a_failure_not_a_skip(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            (root / "lib/mazda/internal_include/mazda/internal_contracts.hpp").unlink()
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("missing required internal header", output)
        self.assertNotIn("SKIP internal contract access probes", output)

    def test_public_internal_shim_is_rejected_for_top_level_access(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            public_shim = root / "lib/mazda/include/mazda/internal_contracts.hpp"
            public_shim.write_text('#include "mazda/handoff_impl.hpp"\n', encoding="utf-8")
            handoff = root / "lib/mazda/internal_include/mazda/handoff_impl.hpp"
            handoff.write_text(
                "#pragma once\n"
                "namespace mazda { struct InternalContract {}; }\n",
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("top-level accessible", output)
        self.assertIn("top-level inaccessibility", output)

    def test_direct_vehicle_core_frame_consumer_is_checked(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            (root / "lib/vehicle_core/include/vehicle_core/frame.hpp").unlink()
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("CMake consumer probe build failed", output)

    def test_parent_directory_name_is_not_a_private_include_match(self) -> None:
        with tempfile.TemporaryDirectory(prefix="internal_include-parent-") as directory:
            result = run_checker(self.copy_fixture(Path(directory)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_response_file_include_arguments_are_inspected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = Path(directory)
            response_file = root / "consumer.rsp"
            response_file.write_text(
                f'-I"{root / "lib/mazda/internal_include"}"\n', encoding="utf-8"
            )
            tokens, errors = _compile_database_command(
                {"directory": str(root), "arguments": ["c++", "@consumer.rsp", "consumer.cpp"]}
            )
            include_dirs = _include_directory_arguments(tokens, root)
        self.assertEqual(errors, [])
        self.assertIn((root / "lib/mazda/internal_include").resolve(), include_dirs)

    def test_msvc_dependency_inspection_is_explicitly_unsupported(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            failures = check_public_headers(root, ("cl",), Path(directory) / "work")
        self.assertTrue(
            any("MSVC dependency inspection is unsupported" in failure for failure in failures),
            failures,
        )

    def test_transitive_forbidden_include_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            frame = root / "lib/vehicle_core/include/vehicle_core/frame.hpp"
            frame.write_text("#pragma once\nstruct RawCanFrame {};\n", encoding="utf-8")
            facade = root / "lib/mazda/include/mazda/facade_contracts.hpp"
            facade.write_text(
                '#include "vehicle_core/frame.hpp"\n' + facade.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("raw frame", output)

    def test_exported_private_path_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            cmake = root / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "\ntarget_include_directories(vehicle_telemetry_contracts INTERFACE\n"
                + "  ${CMAKE_CURRENT_SOURCE_DIR}/lib/mazda/private_include)\n",
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("exported private/internal include path", output)

    def test_exported_internal_path_is_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            cmake = root / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "\ntarget_include_directories(vehicle_telemetry_contracts INTERFACE\n"
                + "  ${CMAKE_CURRENT_SOURCE_DIR}/lib/mazda/internal_include)\n",
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("exported private/internal include path", output)

    def test_conditional_forbidden_include_uses_consumer_compile_flags(self) -> None:
        with tempfile.TemporaryDirectory(prefix="header-boundary-test-") as directory:
            root = self.copy_fixture(Path(directory))
            facade = root / "lib/mazda/include/mazda/facade_contracts.hpp"
            facade.write_text(
                "#pragma once\n"
                "#if defined(MAZDA_ENABLE_RAW_FRAME)\n"
                '#include "vehicle_core/frame.hpp"\n'
                "#endif\n"
                + facade.read_text(encoding="utf-8").replace("#pragma once\n", "", 1),
                encoding="utf-8",
            )
            cmake = root / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "\ntarget_compile_definitions(vehicle_telemetry_contracts INTERFACE"
                " MAZDA_ENABLE_RAW_FRAME)\n",
                encoding="utf-8",
            )
            result = run_checker(root)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("actual compile flags", output)
        self.assertIn("raw frame", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--compiler")
    parser.add_argument("--cmake", default="cmake")
    options, remaining = parser.parse_known_args()
    CHECKER_COMPILER = options.compiler
    CHECKER_CMAKE = options.cmake
    sys.argv[1:] = remaining
    unittest.main()
