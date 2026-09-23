"""Persistent tests for the toolchain checker's diagnostics and resolution."""

from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Tuple
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import check_toolchain  # noqa: E402


class ToolchainCheckerTests(unittest.TestCase):
    def run_check(self, scope: str) -> Tuple[int, str]:
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            result = check_toolchain.check(scope)
        return result, output.getvalue()

    def test_successful_requirement(self) -> None:
        requirement = check_toolchain.Requirement(
            "fake", "fake", (), frozenset({"test"}), check_toolchain._present, "https://example.invalid"
        )
        with patch.object(check_toolchain, "REQUIREMENTS", (requirement,)), patch.object(
            check_toolchain, "_find_executable", return_value="/tmp/fake"
        ), patch.object(check_toolchain, "_run", return_value=(0, "fake 1.0")):
            result, output = self.run_check("test")
        self.assertEqual(result, 0)
        self.assertIn("Toolchain check passed", output)

    def test_missing_requirement_reports_install_hint(self) -> None:
        requirement = check_toolchain.Requirement(
            "fake", "fake", (), frozenset({"test"}), check_toolchain._present, "https://example.invalid/install"
        )
        with patch.object(check_toolchain, "REQUIREMENTS", (requirement,)), patch.object(
            check_toolchain, "_find_executable", return_value=None
        ):
            result, output = self.run_check("test")
        self.assertEqual(result, 1)
        self.assertIn("'fake' not found", output)
        self.assertIn("https://example.invalid/install", output)

    def test_version_error_reports_actual_and_expected(self) -> None:
        requirement = check_toolchain.Requirement(
            "fake", "fake", (), frozenset({"test"}), check_toolchain._exact((6, 1, 18)), "https://example.invalid"
        )
        with patch.object(check_toolchain, "REQUIREMENTS", (requirement,)), patch.object(
            check_toolchain, "_find_executable", return_value="/tmp/fake"
        ), patch.object(check_toolchain, "_run", return_value=(0, "fake 6.1.17")):
            result, output = self.run_check("test")
        self.assertEqual(result, 1)
        self.assertIn("reported 6.1.17; need exactly 6.1.18", output)

    def test_exact_version_rejects_prerelease_suffix(self) -> None:
        valid, detail = check_toolchain._exact((5, 5, 4))("ESP-IDF v5.5.4-dev")
        self.assertFalse(valid)
        self.assertIn("reported 5.5.4-dev; need exactly 5.5.4", detail)

    def test_cmake_support_includes_minimum_and_modern_legs(self) -> None:
        requirement = next(item for item in check_toolchain.REQUIREMENTS if item.name == "CMake")
        minimum_valid, _ = requirement.check("cmake version 3.20.5")
        modern_valid, _ = requirement.check("cmake version 4.4.3")
        below_minimum, detail = requirement.check("cmake version 3.19.9")
        self.assertTrue(minimum_valid)
        self.assertTrue(modern_valid)
        self.assertFalse(below_minimum)
        self.assertIn("need >= 3.20", detail)

    def test_venv_sibling_tool_is_found_without_path_entry(self) -> None:
        with patch.object(check_toolchain.shutil, "which", return_value=None), patch.object(
            check_toolchain.sys, "executable", "/tmp/venv/bin/python"
        ), patch.object(check_toolchain.sys, "prefix", "/tmp/venv"), patch.object(
            Path, "is_file", side_effect=[True, False, False]
        ), patch.object(Path, "stat") as stat:
            stat.return_value.st_mode = 0o100755
            candidates = check_toolchain._executable_candidates("tool")
        self.assertEqual(candidates, (str(Path("/tmp/venv/bin/tool").resolve()),))

    def test_venv_sibling_tool_precedes_wrong_path_version(self) -> None:
        with patch.object(check_toolchain.shutil, "which", return_value="/usr/bin/tool"), patch.object(
            check_toolchain.sys, "executable", "/tmp/venv/bin/python"
        ), patch.object(check_toolchain.sys, "prefix", "/tmp/venv"), patch.object(
            check_toolchain.sys, "base_prefix", "/usr"
        ), patch.object(Path, "is_file", side_effect=[True, False, False]), patch.object(
            Path, "stat"
        ) as stat:
            stat.return_value.st_mode = 0o100755
            selected = check_toolchain._find_executable("tool")
        self.assertEqual(selected, str(Path("/tmp/venv/bin/tool").resolve()))

    def test_system_python_preserves_path_precedence(self) -> None:
        with patch.object(check_toolchain.shutil, "which", return_value="/usr/local/bin/tool"), patch.object(
            check_toolchain.sys, "executable", "/usr/bin/python3"
        ), patch.object(check_toolchain.sys, "prefix", "/usr"), patch.object(
            check_toolchain.sys, "base_prefix", "/usr"
        ), patch.object(Path, "is_file", return_value=False):
            selected = check_toolchain._find_executable("tool")
        self.assertEqual(selected, "/usr/local/bin/tool")

    def test_system_python_path_precedes_existing_interpreter_tool(self) -> None:
        with patch.object(check_toolchain.shutil, "which", return_value="/usr/local/bin/tool"), patch.object(
            check_toolchain.sys, "executable", "/usr/bin/python3"
        ), patch.object(check_toolchain.sys, "prefix", "/usr"), patch.object(
            check_toolchain.sys, "base_prefix", "/usr"
        ), patch.object(Path, "is_file", return_value=True), patch.object(Path, "stat") as stat:
            stat.return_value.st_mode = 0o100755
            selected = check_toolchain._find_executable("tool")
        self.assertEqual(selected, "/usr/local/bin/tool")


if __name__ == "__main__":
    unittest.main()
