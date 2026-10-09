"""Tests for the IRAM placement checker's parsing and placement decisions."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_iram_placement  # noqa: E402


FRAGMENT = """\
# comment: not_a_symbol (noflash)
[mapping:hot]
archive: libhot.a
entries:
    obj:_Z3hotv (noflash)
    obj:_Z4warmv (noflash_text)
    obj:_Z4datav (noflash_data)
    * (noflash)
    other (noflash)
"""

OBJDUMP = """\
Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .iram0.vectors 00000403  40080000  40080000  00001000  2**2
  1 .iram0.text   00010000  40080404  40080404  00001404  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  2 .flash.text   00020000  400d0020  400d0020  00020020  2**2
"""

NM = """\
40080500 T _Z3hotv
400d1000 T _Z4warmv
         U _Z7externv
"""


class IramPlacementCheckerTests(unittest.TestCase):
    def test_fragment_parsing_keeps_code_symbol_entries_only(self) -> None:
        self.assertEqual(
            check_iram_placement.parse_fragment_symbols(FRAGMENT), ["_Z3hotv", "_Z4warmv"]
        )

    def test_section_bounds_come_from_objdump_headers(self) -> None:
        section = check_iram_placement.parse_section(OBJDUMP, ".iram0.text")
        self.assertEqual((section.start, section.size), (0x40080404, 0x10000))
        with self.assertRaises(ValueError):
            check_iram_placement.parse_section(OBJDUMP, ".missing")

    def test_check_reports_missing_and_misplaced_symbols(self) -> None:
        section = check_iram_placement.parse_section(OBJDUMP, ".iram0.text")
        defined = check_iram_placement.parse_nm(NM)
        failures = check_iram_placement.check(["_Z3hotv", "_Z4warmv", "_Z4gonev"], section, defined)
        self.assertEqual(len(failures), 2)
        self.assertIn("not in .iram0.text", failures[0])
        self.assertIn("_Z4warmv", failures[0])
        self.assertEqual(failures[1], "missing from ELF: _Z4gonev")

    def run_main(self, fragment: str) -> tuple:
        outputs = {"-h": OBJDUMP, "--defined-only": NM}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hot.lf"
            path.write_text(fragment, encoding="utf-8")
            output = io.StringIO()
            with patch.object(
                check_iram_placement, "_run", side_effect=lambda command: outputs[command[1]]
            ), redirect_stdout(output), redirect_stderr(output):
                result = check_iram_placement.main(
                    ["--fragment", str(path), "--elf", "app.elf"]
                )
        return result, output.getvalue()

    def test_main_passes_when_all_symbols_are_in_iram(self) -> None:
        result, output = self.run_main("[mapping:m]\narchive: a\nentries:\n    o:_Z3hotv (noflash)\n")
        self.assertEqual(result, 0)
        self.assertIn("passed: 1 symbols", output)

    def test_main_fails_with_actionable_message(self) -> None:
        result, output = self.run_main(FRAGMENT)
        self.assertEqual(result, 1)
        self.assertIn("1 of 2 symbols", output)
        self.assertIn("ldgen ignores entries", output)

    def test_main_rejects_fragment_without_symbol_entries(self) -> None:
        result, output = self.run_main("[mapping:m]\narchive: a\nentries:\n    * (noflash)\n")
        self.assertEqual(result, 1)
        self.assertIn("no symbol entries", output)


if __name__ == "__main__":
    unittest.main()
