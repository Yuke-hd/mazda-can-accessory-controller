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

    def test_local_code_resolves_actual_gcc_helper_substitutions(self) -> None:
        # GCC 14 outlined this helper in the first candidate. Its NS0_ ABI
        # substitutions differ from the fragment parent's NS_ spelling.
        raw = '4013e47c t _ZSt13__invoke_implIvZN5mazda8internal23VehicleTelemetryService21publish_notificationsERKNS0_12VehicleStateERKNS0_11DiagnosticsEySt8optionalImEbbEUlDpRKT_E_JRKNS1_22NotificationDescriptorINS0_16SelectorPositionELt1EEERKNSG_INS0_10ActualGearELt2EEERKNSG_INS0_9TurnStateELt3EEERKNSG_IbLt4EEERKNSG_IbLt5EEERKNSG_IbLt6EEERKNSG_IbLt7EEERKNSG_IbLt8EEERKNSG_IbLt9EEERKNSG_IbLt10EEERKNSG_IbLt11EEERKNSG_IbLt12EEERKNSG_IbLt13EEERKNSG_IbLt14EEERKNSG_IbLt15EEERKNSG_INS0_18FrontWiperPositionELt16EEERKNSG_IbLt17EEEEET_St14__invoke_otherOT0_DpOT1_'
        demangled = '4013e47c t void std::__invoke_impl<void, mazda::internal::VehicleTelemetryService::publish_notifications(mazda::VehicleState const&, mazda::Diagnostics const&, unsigned long long, std::optional<unsigned long>, bool, bool)::{lambda((auto:1 const&)...)#1}, mazda::internal::NotificationDescriptor<mazda::SelectorPosition, (unsigned short)1> const&, mazda::internal::NotificationDescriptor<mazda::ActualGear, (unsigned short)2> const&, mazda::internal::NotificationDescriptor<mazda::TurnState, (unsigned short)3> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)4> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)5> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)6> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)7> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)8> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)9> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)10> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)11> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)12> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)13> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)14> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)15> const&, mazda::internal::NotificationDescriptor<mazda::FrontWiperPosition, (unsigned short)16> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)17> const&>(std::__invoke_other, mazda::internal::VehicleTelemetryService::publish_notifications(mazda::VehicleState const&, mazda::Diagnostics const&, unsigned long long, std::optional<unsigned long>, bool, bool)::{lambda((auto:1 const&)...)#1}&&, mazda::internal::NotificationDescriptor<mazda::SelectorPosition, (unsigned short)1> const&, mazda::internal::NotificationDescriptor<mazda::ActualGear, (unsigned short)2> const&, mazda::internal::NotificationDescriptor<mazda::TurnState, (unsigned short)3> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)4> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)5> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)6> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)7> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)8> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)9> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)10> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)11> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)12> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)13> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)14> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)15> const&, mazda::internal::NotificationDescriptor<mazda::FrontWiperPosition, (unsigned short)16> const&, mazda::internal::NotificationDescriptor<bool, (unsigned short)17> const&)'
        parent = "mazda::internal::VehicleTelemetryService::publish_notifications("
        self.assertEqual(check_iram_placement.local_code_symbols(raw, demangled, [parent]),
                         [raw.split(maxsplit=2)[2]])

    def test_local_code_ignores_static_data_and_checks_table_alignment(self) -> None:
        raw = "40080500 T _Z3hotv\n3ffb0010 b _ZZ3hotvE4data\n"
        names = "40080500 T hot()\n3ffb0010 b hot()::data\n"
        self.assertEqual(check_iram_placement.local_code_symbols(raw, names, ["hot()"]),
                         ["_Z3hotv"])
        with self.assertRaisesRegex(ValueError, "do not align"):
            check_iram_placement.local_code_symbols(raw, names.replace("40080500", "40080501"), ["hot()"])
        with self.assertRaisesRegex(ValueError, "parent not found"):
            check_iram_placement.local_code_symbols(raw, names, ["missing()"])

    def test_local_parent_comments_are_explicit(self) -> None:
        self.assertEqual(check_iram_placement.parse_local_code_parents(
            "# check-local-code: ns::hot(\n# other: ns::cold(\n"), ["ns::hot("])

    def run_main(self, fragment: str, nm: str = NM, demangled: str = "") -> tuple:
        def run_output(command):
            if command[1] == "-h":
                return OBJDUMP
            return demangled if "--demangle" in command else nm
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hot.lf"
            path.write_text(fragment, encoding="utf-8")
            output = io.StringIO()
            with patch.object(
                check_iram_placement, "_run", side_effect=run_output
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

    def test_main_checks_new_tuple_lambda_without_an_exact_fragment_entry(self) -> None:
        fragment = "# check-local-code: hot()\n[mapping:m]\narchive: a\nentries:\n    o:_Z3hotv (noflash)\n"
        name = "_ZZ3hotvENKUlDpRKT_E_clIJnew_descriptor_tupleEEEDaS2_"
        base_names = NM.replace("_Z3hotv", "hot()").replace("_Z4warmv", "warm()")
        for address, expected in (("400d2000", 1), ("40080600", 0)):
            with self.subTest(address=address):
                result, output = self.run_main(
                    fragment, NM + f"{address} t {name}\n",
                    base_names + f"{address} t void invoke<hot()::{{lambda(new tuple)}}>()\n")
                self.assertEqual(result, expected)
                self.assertIn("2 symbols", output)
                if expected:
                    self.assertIn(name, output)


if __name__ == "__main__":
    unittest.main()
