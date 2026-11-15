"""Tests for the round display setup code provisioning script."""

from __future__ import annotations

import csv
import io
import stat
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import provision_display_code as tool  # noqa: E402


class ProvisionDisplayCodeTests(unittest.TestCase):
    def run_tool(self, *args: str) -> tuple[int, str]:
        output = io.StringIO()
        with redirect_stdout(output):
            result = tool.main(list(args))
        return result, output.getvalue()

    def test_generated_code_is_16_uppercase_hex(self) -> None:
        codes = {tool.generate_code() for _ in range(32)}
        self.assertEqual(len(codes), 32)
        for code in codes:
            self.assertRegex(code, r"^[0-9A-F]{16}$")

    def test_normalize_matches_firmware_rules(self) -> None:
        self.assertEqual(tool.normalize_code("01 23-45 67-89ab cdef"), "0123456789ABCDEF")
        for bad in ("0123456789ABCDE", "0123456789ABCDEF0", "0123456789ABCDEG", ""):
            with self.assertRaises(ValueError):
                tool.normalize_code(bad)

    def test_writes_private_nvs_csv_outside_the_repo(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result, printed = self.run_tool("--out", directory, "--code", "0123-4567-89ab-cdef")
            self.assertEqual(result, 0)
            path = Path(directory) / tool.CSV_NAME
            rows = list(csv.reader(path.read_text().splitlines()))
            self.assertEqual(
                rows,
                [
                    ["key", "type", "encoding", "value"],
                    ["display_link", "namespace", "", ""],
                    ["setup_code", "data", "string", "0123456789ABCDEF"],
                ],
            )
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertIn("0123456789ABCDEF", printed)

    def test_refuses_an_output_inside_the_repo(self) -> None:
        result, printed = self.run_tool("--out", str(REPO / "build"), "--code", "0123456789ABCDEF")
        self.assertEqual(result, 2)
        self.assertIn("outside the repository", printed)
        self.assertFalse((REPO / "build" / tool.CSV_NAME).exists())

    def test_rejects_a_bad_code(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result, _ = self.run_tool("--out", directory, "--code", "XYZ")
            self.assertEqual(result, 2)
            self.assertFalse((Path(directory) / tool.CSV_NAME).exists())


if __name__ == "__main__":
    unittest.main()
