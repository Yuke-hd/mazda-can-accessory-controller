"""Deterministic and non-mutating compiler-manifest generation checks."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "generate_controller_signal_catalog", ROOT / "tools/generate_controller_signal_catalog.py")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)

EXPORTED = '{"vehicle.example":{"type":"enum","capabilities":["read"],"choices":["unknown","on"]}}'
EXPECTED = b'''{
  "version": 1,
  "source": "components/mazda_telemetry/private_include/mazda/signal_catalog.hpp",
  "signals": {
    "vehicle.example": {
      "type": "enum",
      "capabilities": [
        "read"
      ],
      "choices": [
        "unknown",
        "on"
      ]
    }
  }
}
'''


class ControllerSignalCatalogGenerationTests(unittest.TestCase):
    def test_manifest_has_independent_expected_shape(self):
        self.assertEqual(generator.render_manifest(EXPORTED), EXPECTED)

    def test_regeneration_is_byte_identical_and_check_does_not_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "catalog.json"
            exporter = Path(temporary) / "exporter"
            result = subprocess.CompletedProcess([str(exporter)], 0, EXPORTED, "")
            with patch.object(generator.subprocess, "run", return_value=result) as run:
                self.assertTrue(generator.generate(exporter, output, check=False))
                self.assertEqual(output.read_bytes(), EXPECTED)
                self.assertTrue(generator.generate(exporter, output, check=False))
                self.assertEqual(output.read_bytes(), EXPECTED)
                before = output.stat().st_mtime_ns
                self.assertTrue(generator.generate(exporter, output, check=True))
                self.assertEqual(output.stat().st_mtime_ns, before)
                self.assertEqual(run.call_args.args[0], [str(exporter.resolve())])

    def test_stale_and_missing_checks_never_rewrite_or_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "catalog.json"
            result = subprocess.CompletedProcess([], 0, EXPORTED, "")
            with patch.object(generator.subprocess, "run", return_value=result):
                self.assertFalse(generator.generate(Path("exporter"), output, check=True))
                self.assertFalse(output.exists())
                output.write_bytes(b"stale\n")
                before = output.stat().st_mtime_ns
                self.assertFalse(generator.generate(Path("exporter"), output, check=True))
                self.assertEqual(output.read_bytes(), b"stale\n")
                self.assertEqual(output.stat().st_mtime_ns, before)

    def test_invalid_export_does_not_replace_prior_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "catalog.json"
            output.write_bytes(EXPECTED)
            for exported in ("invalid json", "[]"):
                result = subprocess.CompletedProcess([], 0, exported, "")
                with self.subTest(exported=exported), patch.object(
                        generator.subprocess, "run", return_value=result):
                    with self.assertRaises(ValueError):
                        generator.generate(Path("exporter"), output, check=False)
                    self.assertEqual(output.read_bytes(), EXPECTED)


if __name__ == "__main__":
    unittest.main()
