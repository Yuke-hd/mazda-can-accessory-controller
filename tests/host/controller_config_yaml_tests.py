"""Run compiler regressions with canonical C++ loading of every valid output."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))
sys.path.insert(0, str(REPOSITORY / "tests" / "tools"))

from compile_controller_config_test import (
    ControllerConfigCompilerTests,
    PRODUCTION_YAML,
    SIGNAL_CATALOG,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, required=True)
    args = parser.parse_args()

    class CanonicalLoaderTests(ControllerConfigCompilerTests):
        def assert_loads(self, payload: bytes) -> None:
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "compiled.json"
                path.write_bytes(payload)
                result = subprocess.run(
                    [str(args.loader), str(path)], capture_output=True, text=True, check=False
                )
                self.assertEqual(result.returncode, 0, result.stderr)

        def run_compiler(
            self, source: str, output_name: str = "output.json"
        ) -> tuple[subprocess.CompletedProcess[str], bytes | None]:
            result, payload = super().run_compiler(source, output_name)
            if result.returncode == 0:
                self.assertIsNotNone(payload)
                self.assert_loads(payload)
            return result, payload

        def test_production_yaml_loads_in_canonical_cpp_loader(self) -> None:
            result, _ = self.run_compiler(PRODUCTION_YAML.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, result.stderr)

        def test_door_liftgate_example_loads_in_canonical_cpp_loader(self) -> None:
            example = (
                REPOSITORY
                / "docs"
                / "specs"
                / "configuration"
                / "examples"
                / "door-liftgate-open-actions-v1.yaml"
            )
            result, _ = self.run_compiler(example.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, result.stderr)

        def test_manifest_matches_full_cpp_catalog_metadata(self) -> None:
            result = subprocess.run(
                [str(args.loader), "--catalog"], capture_output=True, text=True, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(result.stdout),
                json.loads(SIGNAL_CATALOG.read_text(encoding="utf-8"))["signals"],
            )

        def test_actual_gear_example_loads_in_canonical_cpp_loader(self) -> None:
            example = (
                REPOSITORY
                / "docs"
                / "specs"
                / "configuration"
                / "examples"
                / "actual-gear-actions-v1.yaml"
            )
            result, _ = self.run_compiler(example.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, result.stderr)

        def test_indicator_lamp_example_loads_in_canonical_cpp_loader(self) -> None:
            example = (
                REPOSITORY
                / "docs"
                / "specs"
                / "configuration"
                / "examples"
                / "indicator-lamp-actions.yaml"
            )
            result, _ = self.run_compiler(example.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, result.stderr)

    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CanonicalLoaderTests)
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
