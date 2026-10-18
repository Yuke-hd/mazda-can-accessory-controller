"""Tests for the output catalog and limits header generator."""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
GENERATOR = REPOSITORY / "tools" / "generate_output_catalog.py"
GENERATION_MODULE = REPOSITORY / "tools" / "output_catalog.cmake"
LED_DESCRIPTOR = REPOSITORY / "components" / "local_argb_actions" / "output_catalog.yaml"
WEACT_BOARD_FILE = REPOSITORY / "firmware" / "weact-can485-v1.1" / "output_catalog.cmake"
SPEC = REPOSITORY / "docs" / "specs" / "companion" / "output-catalog.md"
HEADER_RELATIVE = Path("controller_config") / "generated" / "output_limits.hpp"

MINIMAL_DESCRIPTOR = """\
descriptor_format: 1
sink: test_sink
outputs:
  - type: test_output
    fields:
      - {name: action, kind: action, required: true}
"""


def reference_catalog() -> bytes:
    """The WeAct document from the spec's Synthetic reference vectors section."""
    text = SPEC.read_text(encoding="utf-8")
    section = text.split("## Synthetic reference vectors", 1)[1]
    match = re.search(r"```json\n(.*?)\n```", section, re.DOTALL)
    assert match is not None, "reference vector block missing from the spec"
    return match.group(1).encode("utf-8")


class GeneratorRun:
    def __init__(self, result: subprocess.CompletedProcess[str], catalog: Path, header: Path) -> None:
        self.result = result
        self.catalog = catalog
        self.header = header

    @property
    def returncode(self) -> int:
        return self.result.returncode

    @property
    def stderr(self) -> str:
        return self.result.stderr


class OutputCatalogGeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)

    def tearDown(self) -> None:
        self._directory.cleanup()

    def write_descriptor(self, name: str, source: str) -> Path:
        path = self.root / name
        path.write_text(textwrap.dedent(source), encoding="utf-8")
        return path

    def generate(
        self,
        descriptors: list[Path],
        board: list[str] | None = None,
        output: str = "out",
    ) -> GeneratorRun:
        output_dir = self.root / output
        catalog = output_dir / "output_catalog.json"
        header = output_dir / "generated" / HEADER_RELATIVE
        command = [sys.executable, str(GENERATOR)]
        for descriptor in descriptors:
            command += ["--descriptor", str(descriptor)]
        for value in board or []:
            command += ["--board", value]
        command += ["--catalog", str(catalog), "--limits-header", str(header)]
        result = subprocess.run(command, cwd=self.root, capture_output=True, text=True, check=False)
        return GeneratorRun(result, catalog, header)

    def generate_weact(self, output: str = "out") -> GeneratorRun:
        return self.generate([LED_DESCRIPTOR], ["strip_pixels=100"], output)

    def assert_rejected(self, run: GeneratorRun, *fragments: str) -> None:
        self.assertNotEqual(run.returncode, 0, "generator accepted invalid input")
        for fragment in fragments:
            self.assertIn(fragment, run.stderr)
        self.assertFalse(run.catalog.exists(), "catalog written on error")
        self.assertFalse(run.header.exists(), "limits header written on error")

    # Reference vector and header -------------------------------------------------

    def test_weact_catalog_matches_reference_vector(self) -> None:
        run = self.generate_weact()
        self.assertEqual(run.returncode, 0, run.stderr)
        expected = reference_catalog()
        self.assertEqual(len(expected), 1518)
        self.assertEqual(run.catalog.read_bytes(), expected)

    def test_weact_header_layout(self) -> None:
        run = self.generate_weact()
        self.assertEqual(run.returncode, 0, run.stderr)
        header = run.header.read_text(encoding="utf-8")
        includes = re.findall(r"^#include\s+(\S+)", header, re.MULTILINE)
        self.assertEqual(sorted(includes), ["<array>", "<cstdint>", "<string_view>"])
        self.assertIn("#pragma once", header)
        self.assertIn("namespace controller_config::generated {", header)
        self.assertIn(
            "inline constexpr std::array<std::string_view, 4> kOutputTypeNames{\n"
            '    "led_effect", "led_fill", "led_transient", "led_solid"};\n',
            header,
        )
        self.assertIn(
            "namespace led_transient {\n"
            "inline constexpr std::int32_t kZonePixels = 100;\n"
            "inline constexpr std::array<std::string_view, 3> kZoneDirections{\n"
            '    "start_to_end", "end_to_start", "center_out"};\n'
            "inline constexpr std::int32_t kColorMin = 0;\n"
            "inline constexpr std::int32_t kColorMax = 255;\n"
            "inline constexpr std::int32_t kDurationMsMin = 1;\n"
            "inline constexpr std::int32_t kDurationMsMax = 5000;\n"
            "inline constexpr std::int32_t kPriorityMin = 0;\n"
            "inline constexpr std::int32_t kPriorityMax = 255;\n"
            "inline constexpr std::int32_t kPriorityDefault = 100;\n"
            "} // namespace led_transient\n",
            header,
        )
        self.assertIn(
            "inline constexpr std::array<std::string_view, 3> kEffectValues{\n"
            '    "left_turn", "right_turn", "brake"};\n',
            header,
        )
        self.assertIn(
            "namespace led_solid {\n"
            "inline constexpr std::array<std::string_view, 2> kRuleTypes{\n"
            '    "state", "sampled_state"};\n',
            header,
        )
        self.assertNotRegex(header, r"namespace led_(effect|fill|transient) \{[^}]*kRuleTypes")

    def test_header_compiles_as_cxx17(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("no C++ compiler")
        run = self.generate_weact()
        self.assertEqual(run.returncode, 0, run.stderr)
        source = self.root / "use.cpp"
        source.write_text(
            "#include <controller_config/generated/output_limits.hpp>\n"
            "namespace generated = controller_config::generated;\n"
            "static_assert(generated::kOutputTypeNames.size() == 4);\n"
            "static_assert(generated::led_fill::kZonePixels == 100);\n"
            "static_assert(generated::led_effect::kPriorityDefault == 100);\n"
            "static_assert(generated::led_solid::kRuleTypes[1] == \"sampled_state\");\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only",
             "-I", str(run.header.parents[2]), str(source)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_output_is_deterministic_and_location_independent(self) -> None:
        first = self.generate_weact("first")
        second = self.generate_weact("second/nested")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.catalog.read_bytes(), second.catalog.read_bytes())
        self.assertEqual(first.header.read_bytes(), second.header.read_bytes())
        for path in (first.catalog, first.header):
            content = path.read_text(encoding="utf-8")
            self.assertNotIn(str(REPOSITORY), content)
            self.assertNotIn(str(self.root), content)
            self.assertNotIn(socket.gethostname(), content)
            self.assertNotRegex(content, r"\d{4}-\d{2}-\d{2}|\d{2}:\d{2}:\d{2}")

    def test_empty_document_is_valid(self) -> None:
        descriptor = self.write_descriptor("empty.yaml", "descriptor_format: 1\nsink: none\noutputs: []\n")
        run = self.generate([descriptor])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.catalog.read_bytes(), b'{"outputs":[]}')
        self.assertIn("std::array<std::string_view, 0> kOutputTypeNames{};", run.header.read_text())

    def test_aliases_are_expanded(self) -> None:
        descriptor = self.write_descriptor("alias.yaml", """\
            descriptor_format: 1
            sink: test_sink
            outputs:
              - type: first
                fields:
                  - &level {name: level, kind: integer, required: true, min: 1, max: 9}
              - type: second
                fields: [*level]
            """)
        run = self.generate([descriptor])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.catalog.read_text().count('"name":"level","kind":"integer"'), 2)

    # Multiple descriptors and board values ---------------------------------------

    def test_descriptors_keep_command_line_order(self) -> None:
        second = self.write_descriptor("b.yaml", MINIMAL_DESCRIPTOR.replace("test_output", "zeta"))
        first = self.write_descriptor("a.yaml", MINIMAL_DESCRIPTOR.replace("test_output", "omega"))
        run = self.generate([second, first])
        self.assertEqual(run.returncode, 0, run.stderr)
        catalog = run.catalog.read_text()
        self.assertLess(catalog.index('"zeta"'), catalog.index('"omega"'))
        self.assertIn('{"zeta", "omega"}', run.header.read_text().replace("\n    ", ""))

    def test_duplicate_type_across_descriptors_is_rejected(self) -> None:
        first = self.write_descriptor("first.yaml", MINIMAL_DESCRIPTOR)
        second = self.write_descriptor("second.yaml", MINIMAL_DESCRIPTOR.replace("test_sink", "other"))
        run = self.generate([first, second])
        self.assert_rejected(run, "second.yaml", "outputs[0].type", "test_output", "first.yaml")

    def test_duplicate_type_within_descriptor_is_rejected(self) -> None:
        descriptor = self.write_descriptor("dup.yaml", MINIMAL_DESCRIPTOR + """\
  - type: test_output
    fields: []
""")
        self.assert_rejected(self.generate([descriptor]), "dup.yaml", "outputs[1].type")

    def test_board_reference_is_resolved(self) -> None:
        descriptor = self.write_descriptor("board.yaml", """\
            descriptor_format: 1
            sink: test_sink
            outputs:
              - type: strip
                fields:
                  - {name: zone, kind: zone, required: true, pixels: {board: strip_pixels},
                     directions: [start_to_end]}
            """)
        run = self.generate([descriptor], ["strip_pixels=42"])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('"pixels":42', run.catalog.read_text())
        self.assertIn("kZonePixels = 42;", run.header.read_text())

    def test_unresolved_board_reference_is_rejected(self) -> None:
        run = self.generate([LED_DESCRIPTOR])
        self.assert_rejected(run, "output_catalog.yaml", "outputs[1].fields[1].pixels", "strip_pixels")

    def test_unused_board_value_is_rejected(self) -> None:
        run = self.generate([LED_DESCRIPTOR], ["strip_pixels=100", "relay_count=2"])
        self.assert_rejected(run, "relay_count")

    def test_malformed_board_values_are_rejected(self) -> None:
        for value in ("strip_pixels", "strip_pixels=", "strip_pixels=0x10", "strip_pixels=1.5",
                      "Strip=1", "strip_pixels=2147483648"):
            with self.subTest(value=value):
                self.assert_rejected(self.generate([LED_DESCRIPTOR], [value]), "--board")

    def test_duplicate_board_value_is_rejected(self) -> None:
        run = self.generate([LED_DESCRIPTOR], ["strip_pixels=100", "strip_pixels=100"])
        self.assert_rejected(run, "--board", "strip_pixels")

    def test_existing_outputs_are_untouched_on_error(self) -> None:
        good = self.generate_weact()
        self.assertEqual(good.returncode, 0, good.stderr)
        catalog = good.catalog.read_bytes()
        header = good.header.read_bytes()
        broken = self.write_descriptor("broken.yaml", "descriptor_format: 2\nsink: x\noutputs: []\n")
        bad = self.generate([broken])
        self.assertNotEqual(bad.returncode, 0)
        self.assertEqual(good.catalog.read_bytes(), catalog)
        self.assertEqual(good.header.read_bytes(), header)

    # Descriptor rules ------------------------------------------------------------

    def test_descriptor_rule_violations_name_file_and_path(self) -> None:
        field_cases = {
            "unknown kind": ("{name: f, kind: float, required: true}", "fields[0].kind"),
            "missing required flag": ("{name: f, kind: action}", "fields[0].required"),
            "non-boolean required": ("{name: f, kind: action, required: 1}", "fields[0].required"),
            "unknown field key": ("{name: f, kind: action, required: true, label: F}", "fields[0].label"),
            "action attribute": ("{name: f, kind: action, required: true, min: 0}", "fields[0].min"),
            "missing min": ("{name: f, kind: integer, required: true, max: 1}", "fields[0].min"),
            "boolean min": ("{name: f, kind: integer, required: true, min: true, max: 1}", "fields[0].min"),
            "float max": ("{name: f, kind: integer, required: true, min: 0, max: 1.5}", "fields[0].max"),
            "min above max": ("{name: f, kind: integer, required: true, min: 2, max: 1}", "fields[0].min"),
            "int32 overflow": ("{name: f, kind: integer, required: true, min: 0, max: 2147483648}",
                               "fields[0].max"),
            "default on required": ("{name: f, kind: integer, required: true, min: 0, max: 9, default: 1}",
                                    "fields[0].default"),
            "optional without default": ("{name: f, kind: integer, required: false, min: 0, max: 9}",
                                         "fields[0].default"),
            "default out of range": ("{name: f, kind: integer, required: false, min: 0, max: 9, default: 10}",
                                     "fields[0].default"),
            "default on enum": ("{name: f, kind: enum, required: false, values: [a], default: 0}",
                                "fields[0].default"),
            "empty enum": ("{name: f, kind: enum, required: true, values: []}", "fields[0].values"),
            "duplicate enum value": ("{name: f, kind: enum, required: true, values: [a, a]}",
                                     "fields[0].values[1]"),
            "non snake enum": ("{name: f, kind: enum, required: true, values: [Left]}", "fields[0].values[0]"),
            "rgb missing max": ("{name: f, kind: rgb, required: true, min: 0}", "fields[0].max"),
            "zone missing directions": ("{name: f, kind: zone, required: true, pixels: 10}",
                                        "fields[0].directions"),
            "zone empty pixels": ("{name: f, kind: zone, required: true, pixels: 0, directions: [a]}",
                                  "fields[0].pixels"),
            "bad board reference": ("{name: f, kind: zone, required: true, pixels: {board: 1}, directions: [a]}",
                                    "fields[0].pixels"),
            "board reference extra key": (
                "{name: f, kind: zone, required: true, pixels: {board: n, x: 1}, directions: [a]}",
                "fields[0].pixels"),
            "non snake name": ("{name: Color, kind: action, required: true}", "fields[0].name"),
            "long name": ("{name: " + "a" * 32 + ", kind: action, required: true}", "fields[0].name"),
            "missing name": ("{kind: action, required: true}", "fields[0].name"),
        }
        for label, (field, path) in field_cases.items():
            with self.subTest(label):
                descriptor = self.write_descriptor("rule.yaml", f"""\
                    descriptor_format: 1
                    sink: test_sink
                    outputs:
                      - type: out
                        fields:
                          - {field}
                    """)
                self.assert_rejected(self.generate([descriptor]), "rule.yaml", f"outputs[0].{path}")

    def test_duplicate_field_name_is_rejected(self) -> None:
        descriptor = self.write_descriptor("dup.yaml", MINIMAL_DESCRIPTOR + """\
      - {name: action, kind: action, required: true}
""")
        self.assert_rejected(self.generate([descriptor]), "dup.yaml", "outputs[0].fields[1].name")

    def test_colliding_header_constants_are_rejected(self) -> None:
        descriptor = self.write_descriptor("collide.yaml", """\
            descriptor_format: 1
            sink: s
            outputs:
              - type: out
                fields:
                  - {name: a_b, kind: rgb, required: true, min: 0, max: 1}
                  - {name: a__b, kind: rgb, required: true, min: 0, max: 1}
            """)
        self.assert_rejected(self.generate([descriptor]), "collide.yaml", "outputs[0]", "kABMin")

    def test_reserved_cxx_namespace_type_is_rejected(self) -> None:
        for name in ("switch", "std"):
            with self.subTest(name=name):
                descriptor = self.write_descriptor(
                    "reserved.yaml", f"descriptor_format: 1\nsink: s\noutputs:\n  - {{type: {name}, fields: []}}\n")
                self.assert_rejected(self.generate([descriptor]), "reserved.yaml", "outputs[0].type")

    def test_unhashable_key_is_rejected_without_traceback(self) -> None:
        descriptor = self.write_descriptor(
            "complex_key.yaml", "descriptor_format: 1\nsink: s\n? [a]\n: 1\noutputs: []\n")
        run = self.generate([descriptor])
        self.assert_rejected(run, "complex_key.yaml", "malformed YAML")
        self.assertNotIn("Traceback", run.stderr)

    def test_top_level_and_output_violations_are_rejected(self) -> None:
        cases = {
            "format 2": ("descriptor_format: 2\nsink: s\noutputs: []\n", "descriptor_format"),
            "format string": ("descriptor_format: '1'\nsink: s\noutputs: []\n", "descriptor_format"),
            "missing sink": ("descriptor_format: 1\noutputs: []\n", "sink"),
            "bad sink": ("descriptor_format: 1\nsink: Led\noutputs: []\n", "sink"),
            "missing outputs": ("descriptor_format: 1\nsink: s\n", "outputs"),
            "outputs mapping": ("descriptor_format: 1\nsink: s\noutputs: {}\n", "outputs"),
            "unknown top key": ("descriptor_format: 1\nsink: s\noutputs: []\nextra: 1\n", "extra"),
            "unknown output key": (
                "descriptor_format: 1\nsink: s\noutputs:\n  - {type: t, fields: [], icon: x}\n",
                "outputs[0].icon"),
            "missing fields": ("descriptor_format: 1\nsink: s\noutputs:\n  - {type: t}\n", "outputs[0].fields"),
            "bad type": ("descriptor_format: 1\nsink: s\noutputs:\n  - {type: 1t, fields: []}\n",
                         "outputs[0].type"),
            "empty rule types": (
                "descriptor_format: 1\nsink: s\noutputs:\n  - {type: t, rule_types: [], fields: []}\n",
                "outputs[0].rule_types"),
            "bad rule type": (
                "descriptor_format: 1\nsink: s\noutputs:\n  - {type: t, rule_types: [State], fields: []}\n",
                "outputs[0].rule_types[0]"),
            "duplicate yaml key": ("descriptor_format: 1\nsink: s\nsink: t\noutputs: []\n", "duplicate"),
            "not a mapping": ("- 1\n", "$"),
            "malformed yaml": ("descriptor_format: [1\n", "malformed.yaml"),
        }
        for label, (source, fragment) in cases.items():
            with self.subTest(label):
                descriptor = self.write_descriptor("malformed.yaml", source)
                self.assert_rejected(self.generate([descriptor]), "malformed.yaml", fragment)

    def test_missing_descriptor_is_rejected(self) -> None:
        self.assert_rejected(self.generate([self.root / "absent.yaml"]), "absent.yaml")

    # Limits ----------------------------------------------------------------------

    def test_limits_are_enforced(self) -> None:
        action = "{name: action, kind: action, required: true}"
        many_outputs = "\n".join(f"  - {{type: t{index}, fields: []}}" for index in range(33))
        many_fields = ", ".join(f"{{name: f{index}, kind: action, required: true}}" for index in range(17))
        many_values = ", ".join(f"v{index}" for index in range(33))
        cases = {
            "33 output types": (f"outputs:\n{many_outputs}\n", "outputs[32]"),
            "17 fields": (f"outputs:\n  - {{type: t, fields: [{many_fields}]}}\n", "outputs[0].fields"),
            "33 enum values": (
                f"outputs:\n  - {{type: t, fields: [{{name: f, kind: enum, required: true, "
                f"values: [{many_values}]}}]}}\n",
                "outputs[0].fields[0].values"),
            "33 directions": (
                f"outputs:\n  - {{type: t, fields: [{{name: f, kind: zone, required: true, pixels: 1, "
                f"directions: [{many_values}]}}]}}\n",
                "outputs[0].fields[0].directions"),
            "33 rule types": (
                f"outputs:\n  - {{type: t, rule_types: [{many_values}], fields: [{action}]}}\n",
                "outputs[0].rule_types"),
            "32-byte type": (f"outputs:\n  - {{type: {'t' * 32}, fields: []}}\n", "outputs[0].type"),
        }
        for label, (outputs, path) in cases.items():
            with self.subTest(label):
                descriptor = self.write_descriptor("limits.yaml", f"descriptor_format: 1\nsink: s\n{outputs}")
                self.assert_rejected(self.generate([descriptor]), "limits.yaml", path)

    def test_name_of_31_bytes_is_accepted(self) -> None:
        descriptor = self.write_descriptor(
            "long.yaml", f"descriptor_format: 1\nsink: s\noutputs:\n  - {{type: {'t' * 31}, fields: []}}\n")
        self.assertEqual(self.generate([descriptor]).returncode, 0)

    def test_output_type_limit_spans_descriptors(self) -> None:
        descriptors = []
        for start in (0, 16, 32):
            count = 16 if start < 32 else 1
            outputs = "\n".join(f"  - {{type: t{start + index}, fields: []}}" for index in range(count))
            descriptors.append(self.write_descriptor(f"d{start}.yaml",
                                                     f"descriptor_format: 1\nsink: s\noutputs:\n{outputs}\n"))
        self.assert_rejected(self.generate(descriptors), "d32.yaml", "outputs[0]", "32")

    def test_more_than_four_descriptors_are_rejected(self) -> None:
        descriptors = [
            self.write_descriptor(f"d{index}.yaml", MINIMAL_DESCRIPTOR.replace("test_output", f"t{index}"))
            for index in range(5)
        ]
        self.assert_rejected(self.generate(descriptors), "--descriptor", "4")

    def test_document_size_limit_is_enforced(self) -> None:
        values = ", ".join(f"value_{index:02d}_{'x' * 22}" for index in range(32))
        fields = ", ".join(
            f"{{name: field_{index:02d}, kind: enum, required: true, values: [{values}]}}" for index in range(16))
        descriptor = self.write_descriptor(
            "big.yaml", f"descriptor_format: 1\nsink: s\noutputs:\n  - {{type: t, fields: [{fields}]}}\n")
        self.assert_rejected(self.generate([descriptor]), "big.yaml", "16384")


class OutputCatalogBuildIntegrationTests(unittest.TestCase):
    cmake = "cmake"

    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.descriptor = self.source / "output_catalog.yaml"
        self.descriptor.write_text(LED_DESCRIPTOR.read_text(encoding="utf-8"), encoding="utf-8")
        self.board_file = self.source / "board.cmake"
        self.board_file.write_text(
            f'set(OUTPUT_CATALOG_DESCRIPTORS "{self.descriptor.as_posix()}")\n'
            'set(OUTPUT_CATALOG_BOARD "strip_pixels=100")\n',
            encoding="utf-8",
        )
        (self.source / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.20)
project(output_catalog_integration NONE)
include("{GENERATION_MODULE.as_posix()}")
output_catalog_generate(BOARD_FILE "{self.board_file.as_posix()}"
                        OUTPUT_DIR "${{CMAKE_CURRENT_BINARY_DIR}}/output_catalog")
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/paths.txt"
     "${{OUTPUT_CATALOG_JSON}}\\n${{OUTPUT_CATALOG_INCLUDE_DIR}}\\n")
""", encoding="utf-8")

    def tearDown(self) -> None:
        self._directory.cleanup()

    def run_cmake(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([self.cmake, *arguments], capture_output=True, text=True, check=False)

    def configure(self) -> subprocess.CompletedProcess[str]:
        return self.run_cmake("-S", str(self.source), "-B", str(self.build))

    def outputs(self) -> tuple[Path, Path]:
        catalog, include_dir = (self.build / "paths.txt").read_text(encoding="utf-8").splitlines()
        return Path(catalog), Path(include_dir) / HEADER_RELATIVE

    def test_malformed_descriptor_fails_configuration(self) -> None:
        self.descriptor.write_text("descriptor_format: 2\nsink: s\noutputs: []\n", encoding="utf-8")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("descriptor_format", result.stdout + result.stderr)

    def test_configure_generates_and_build_regenerates(self) -> None:
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        catalog, header = self.outputs()
        self.assertEqual(catalog.read_bytes(), reference_catalog())
        self.assertIn("kZonePixels = 100;", header.read_text(encoding="utf-8"))

        self.board_file.write_text(
            self.board_file.read_text(encoding="utf-8").replace("strip_pixels=100", "strip_pixels=60"),
            encoding="utf-8")
        result = self.run_cmake("--build", str(self.build))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('"pixels":60', catalog.read_text(encoding="utf-8"))
        self.assertIn("kZonePixels = 60;", header.read_text(encoding="utf-8"))

        self.descriptor.write_text(
            self.descriptor.read_text(encoding="utf-8").replace("max: 5000", "max: 4000"), encoding="utf-8")
        result = self.run_cmake("--build", str(self.build))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("kDurationMsMax = 4000;", header.read_text(encoding="utf-8"))

    def test_second_generation_call_is_rejected(self) -> None:
        cmake_lists = self.source / "CMakeLists.txt"
        cmake_lists.write_text(cmake_lists.read_text(encoding="utf-8") + f"""
output_catalog_generate(BOARD_FILE "{self.board_file.as_posix()}"
                        OUTPUT_DIR "${{CMAKE_CURRENT_BINARY_DIR}}/second")
""", encoding="utf-8")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("once per project", result.stdout + result.stderr)

    def test_weact_board_file_selects_led_descriptor(self) -> None:
        self.board_file = WEACT_BOARD_FILE
        (self.source / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.20)
project(weact_board_file NONE)
include("{WEACT_BOARD_FILE.as_posix()}")
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/board.txt"
     "${{OUTPUT_CATALOG_DESCRIPTORS}}\\n${{OUTPUT_CATALOG_BOARD}}\\n")
""", encoding="utf-8")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        descriptors, board = (self.build / "board.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(Path(descriptors).resolve(), LED_DESCRIPTOR.resolve())
        self.assertEqual(board, "strip_pixels=100")


class GeneratedFilesAreNotCheckedInTests(unittest.TestCase):
    def test_no_generated_output_is_tracked(self) -> None:
        result = subprocess.run(["git", "ls-files"], cwd=REPOSITORY, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            self.skipTest("not a git checkout")
        tracked = result.stdout.splitlines()
        self.assertFalse([path for path in tracked if path.endswith("output_limits.hpp")])
        self.assertFalse([path for path in tracked if Path(path).name == "output_catalog.json"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    options, unittest_args = parser.parse_known_args()
    OutputCatalogBuildIntegrationTests.cmake = options.cmake
    unittest.main(argv=[__file__, *unittest_args])
