"""Tests for the host-side YAML controller configuration compiler."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
COMPILER = REPOSITORY / "tools" / "compile_controller_config.py"
PRODUCTION_YAML = REPOSITORY / "docs" / "specs" / "configuration" / "examples" / "controller-config-v1.yaml"
PRODUCTION_JSON = REPOSITORY / "docs" / "specs" / "configuration" / "examples" / "controller-config-v1.json"
SIGNAL_CATALOG = REPOSITORY / "tools" / "controller_signal_catalog.json"
SIGNAL_CATALOG_HEADER = (
    REPOSITORY / "components" / "mazda_telemetry" / "private_include" / "mazda" / "signal_catalog.hpp"
)
PERSISTED_NAMES = REPOSITORY / "components" / "controller_config" / "src" / "persisted" / "names.cpp"


class ControllerConfigCompilerTests(unittest.TestCase):
    def run_compiler(
        self, source: str, output_name: str = "output.json"
    ) -> tuple[subprocess.CompletedProcess[str], bytes | None]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path = root / "input.yaml"
            output_path = root / output_name
            input_path.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(COMPILER), str(input_path), str(output_path)],
                cwd=REPOSITORY,
                check=False,
                capture_output=True,
                text=True,
            )
            return result, output_path.read_bytes() if output_path.exists() else None

    def test_valid_yaml_converts_to_canonical_json(self) -> None:
        source = """
        version: 1
        actions:
          - name: left_turn
          - name: rpm_fill
        rules:
          - type: State
            action: left_turn
            signal_key: vehicle.turn_state
            comparison: Equal
            value: left
          - type: range
            action: rpm_fill
            signal_key: vehicle.engine_rpm
            input: {from: 0.0, to: 6500.0}
            output: {from: 0, to: 1}
            freshness: fresh_or_unverified
        outputs:
          - type: led_effect
            action: left_turn
            effect: right_turn
          - type: led_fill
            action: rpm_fill
            zone: {start: 0, length: 100, direction: center_out}
            color: {red: 0, green: 16, blue: 32}
        """
        result, output_path = self.run_compiler(source)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNotNone(output_path)
        self.assertEqual(
            json.loads(output_path.decode("utf-8")),
            {
                "version": 1,
                "actions": [{"name": "left_turn"}, {"name": "rpm_fill"}],
                "rules": [
                    {
                        "type": "state",
                        "action": "left_turn",
                        "signal_key": "vehicle.turn_state",
                        "comparison": "equal",
                        "operand": {"choice": "left"},
                        "freshness": "fresh",
                    },
                    {
                        "type": "range",
                        "action": "rpm_fill",
                        "signal_key": "vehicle.engine_rpm",
                        "input": {"from": 0, "to": 6500},
                        "output": {"from": 0, "to": 1},
                        "freshness": "fresh_or_unverified",
                    },
                ],
                "outputs": [
                    {
                        "type": "led_effect",
                        "action": "left_turn",
                        "effect": "right_turn",
                        "priority": 100,
                    },
                    {
                        "type": "led_fill",
                        "action": "rpm_fill",
                        "zone": {"start": 0, "length": 100, "direction": "center_out"},
                        "color": {"red": 0, "green": 16, "blue": 32},
                        "priority": 100,
                    },
                ],
            },
        )

    def test_malformed_yaml_fails_with_a_useful_diagnostic(self) -> None:
        result, output_path = self.run_compiler("version: [1\n")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("YAML", result.stderr)
        self.assertIsNone(output_path)

    def transient_source(self, output: dict) -> str:
        # JSON mappings are valid YAML and preserve exact integer input values.
        return json.dumps({"version": 1, "actions": [{"name": "action"}], "outputs": [output]})

    def transient_output(self, **overrides: object) -> dict:
        return {
            "type": "led_transient",
            "action": "action",
            "zone": {"start": 0, "length": 10, "direction": "start_to_end"},
            "color": {"red": 255, "green": 16, "blue": 32},
            "duration_ms": 500,
            **overrides,
        }

    def test_transient_duration_safety_boundary_has_field_diagnostic(self) -> None:
        result, canonical = self.run_compiler(
            self.transient_source(self.transient_output(duration_ms=5000))
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(canonical)["outputs"][0]["duration_ms"], 5000)
        result, canonical = self.run_compiler(
            self.transient_source(self.transient_output(duration_ms=5001))
        )
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("outputs[0].duration_ms", result.stderr)
        self.assertIn("1..5000", result.stderr)
        self.assertIsNone(canonical)

    def test_transient_direction_only_variation_is_a_duplicate_physical_target(self) -> None:
        document = {
            "version": 1,
            "actions": [{"name": "action"}],
            "outputs": [
                self.transient_output(),
                self.transient_output(
                    zone={"start": 0, "length": 10, "direction": "end_to_start"}
                ),
            ],
        }
        result, canonical = self.run_compiler(json.dumps(document))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("outputs[1]", result.stderr)
        self.assertIn("output binding target must be unique", result.stderr)
        self.assertIsNone(canonical)

    def test_transient_output_normalizes_alias_and_default_priority(self) -> None:
        output = self.transient_output(
            type="LedTransient",
            zone={"start": 0, "length": 10, "direction": "StartToEnd"},
        )
        result, canonical = self.run_compiler(self.transient_source(output))

        self.assertEqual(result.returncode, 0, result.stderr)
        normalized = json.loads(canonical)["outputs"][0]
        self.assertEqual(normalized, self.transient_output(priority=100))
        self.assertIsInstance(normalized["duration_ms"], int)

    def test_transient_duration_accepts_exact_integer_bounds(self) -> None:
        for duration in (1, 500, 5000, 1.0):
            with self.subTest(duration=duration):
                result, canonical = self.run_compiler(
                    self.transient_source(self.transient_output(duration_ms=duration, priority=255))
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                normalized = json.loads(canonical)["outputs"][0]
                self.assertEqual(normalized["duration_ms"], duration)
                self.assertIsInstance(normalized["duration_ms"], int)
                self.assertEqual(normalized["priority"], 255)
        for scalar, expected in (("5000.000000000000000000", 5000),
                                 ("8.0e+2", 800)):
            with self.subTest(scalar=scalar):
                source = self.transient_source(self.transient_output(duration_ms="DURATION"))
                result, canonical = self.run_compiler(source.replace('"DURATION"', scalar))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(canonical)["outputs"][0]["duration_ms"], expected)

    def test_invalid_transient_durations_have_field_diagnostics(self) -> None:
        for duration in (0, -1, 1.5, True, False, "500", None,
                         5001, 9007199254740992, 18446744073709552):
            with self.subTest(duration=duration):
                result, canonical = self.run_compiler(
                    self.transient_source(self.transient_output(duration_ms=duration))
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("outputs[0].duration_ms", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertIsNone(canonical)

    def test_transient_duration_rejects_fractional_source_before_double_rounding(self) -> None:
        for scalar in ("9007199254740991.1", "800.000000000000000001", "1.0e-100000000"):
            with self.subTest(duration=scalar):
                source = self.transient_source(self.transient_output(duration_ms="DURATION"))
                result, canonical = self.run_compiler(source.replace('"DURATION"', scalar))

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("outputs[0].duration_ms", result.stderr)
                self.assertIn("must be an integer", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertIsNone(canonical)
        source = self.transient_source(self.transient_output(duration_ms="DURATION"))
        result, canonical = self.run_compiler(source.replace('"DURATION"', "1.0e+100000000"))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("outputs[0].duration_ms", result.stderr)
        self.assertIn("must be in 1..5000", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIsNone(canonical)
        for scalar in (".nan", ".inf", "-.inf"):
            with self.subTest(nonfinite=scalar):
                source = self.transient_source(self.transient_output(duration_ms="DURATION"))
                result, canonical = self.run_compiler(source.replace('"DURATION"', scalar))
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("outputs[0].duration_ms", result.stderr)
                self.assertIn("must be an integer", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertIsNone(canonical)

    def test_invalid_transient_fields_retain_existing_diagnostics(self) -> None:
        cases = (
            ({"type": "state"}, "type", "unsupported output type"),
            ({"type": "LedFlash"}, "type", "unsupported type"),
            ({"type": True}, "type", "expected a string"),
            ({"action": "missing"}, "action", "not declared"),
            ({"action": ""}, "action", "must not be empty"),
            ({"action": True}, "action", "expected a string"),
            ({"zone": None}, "zone", "expected a mapping"),
            ({"zone": {"start": 99, "length": 2, "direction": "start_to_end"}}, "zone", "fit within"),
            ({"zone": {"start": True, "length": 1, "direction": "start_to_end"}}, "zone.start", "integer"),
            ({"zone": {"start": 0, "length": 1.5, "direction": "start_to_end"}}, "zone.length", "integer"),
            ({"zone": {"start": 0, "length": 1, "direction": "sideways"}}, "zone.direction", "unsupported"),
            ({"color": None}, "color", "expected a mapping"),
            ({"color": {"red": -1, "green": 0, "blue": 0}}, "color.red", "0..255"),
            ({"color": {"red": 0, "green": 256, "blue": 0}}, "color.green", "0..255"),
            ({"color": {"red": 0, "green": 0, "blue": 1.5}}, "color.blue", "integer"),
            ({"priority": 256}, "priority", "0..255"),
            ({"priority": True}, "priority", "integer"),
            ({"priority": 1.5}, "priority", "integer"),
        )
        for overrides, field, diagnostic in cases:
            with self.subTest(overrides=overrides):
                result, canonical = self.run_compiler(
                    self.transient_source(self.transient_output(**overrides))
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(f"outputs[0].{field}", result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertIsNone(canonical)

    def test_transient_rejects_missing_and_unknown_fields(self) -> None:
        for field in ("type", "action", "zone", "color", "duration_ms"):
            with self.subTest(missing=field):
                output = self.transient_output()
                del output[field]
                result, canonical = self.run_compiler(self.transient_source(output))
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(f"outputs[0].{field}", result.stderr)
                self.assertIn("required field is missing", result.stderr)
                self.assertIsNone(canonical)
        for field in ("effect", "unexpected"):
            with self.subTest(unknown=field):
                result, canonical = self.run_compiler(
                    self.transient_source(self.transient_output(**{field: "brake"}))
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(f"outputs[0].{field}", result.stderr)
                self.assertIn("unknown field", result.stderr)
                self.assertIsNone(canonical)
        for parent, fields in (("zone", ("start", "length", "direction")),
                               ("color", ("red", "green", "blue"))):
            for field in (*fields, "unexpected"):
                with self.subTest(parent=parent, field=field):
                    output = self.transient_output()
                    if field == "unexpected":
                        output[parent][field] = 1
                        diagnostic = "unknown field"
                    else:
                        del output[parent][field]
                        diagnostic = "required field is missing"
                    result, canonical = self.run_compiler(self.transient_source(output))
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(f"outputs[0].{parent}.{field}", result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertIsNone(canonical)

    def test_transient_duplicate_target_ignores_color_duration_and_priority(self) -> None:
        document = {
            "version": 1,
            "actions": [{"name": "action"}],
            "outputs": [
                self.transient_output(),
                self.transient_output(
                    type="LedTransient", duration_ms=1, priority=0,
                    zone={"start": 0, "length": 10, "direction": "StartToEnd"},
                    color={"red": 0, "green": 0, "blue": 0},
                ),
            ],
        }
        result, canonical = self.run_compiler(json.dumps(document))

        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("outputs[1]", result.stderr)
        self.assertIn("output binding target must be unique", result.stderr)
        self.assertIsNone(canonical)

    def test_transient_distinct_targets_and_fill_on_same_zone_are_allowed(self) -> None:
        fill = self.transient_output(type="led_fill")
        del fill["duration_ms"]
        document = {
            "version": 1,
            "actions": [{"name": "action"}, {"name": "other"}],
            "outputs": [
                self.transient_output(),
                self.transient_output(zone={"start": 1, "length": 10, "direction": "start_to_end"}),
                self.transient_output(zone={"start": 0, "length": 11, "direction": "start_to_end"}),
                self.transient_output(action="other"),
                fill,
            ],
        }
        result, canonical = self.run_compiler(json.dumps(document))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(json.loads(canonical)["outputs"]), 5)

    def test_fractional_integer_fields_fail_before_float_rounding(self) -> None:
        template = """
        version: VERSION
        actions: [{name: action}]
        rules: []
        outputs:
          - type: led_fill
            action: action
            zone: {start: START, length: LENGTH, direction: start_to_end}
            color: {red: RED, green: 0, blue: 0}
            priority: PRIORITY
        """
        defaults = {"VERSION": "1", "START": "0", "LENGTH": "1", "RED": "255", "PRIORITY": "255"}
        for field, value, path in (
            ("VERSION", "1.00000001", "version"),
            ("PRIORITY", "255.000001", "outputs[0].priority"),
            ("START", "1.00000001", "outputs[0].zone.start"),
            ("LENGTH", "1.00000001", "outputs[0].zone.length"),
            ("RED", "255.000001", "outputs[0].color.red"),
            ("VERSION", ".nan", "version"),
            ("VERSION", ".inf", "version"),
            ("VERSION", "true", "version"),
        ):
            with self.subTest(field=field, value=value):
                source = template
                for key, replacement in {**defaults, field: value}.items():
                    source = source.replace(key, replacement)
                result, output = self.run_compiler(source)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(path, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertIsNone(output)

    def test_omitted_lists_default_to_empty(self) -> None:
        for fields in (
            "",
            "actions: [{name: action}]\n",
            "rules: []\n",
            "outputs: []\n",
            "actions: []\nrules: []\n",
            "actions: []\noutputs: []\n",
            "rules: []\noutputs: []\n",
        ):
            with self.subTest(fields=fields):
                result, output = self.run_compiler("version: 1\n" + fields)
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(output)
                self.assertEqual(document["rules"], [])
                self.assertEqual(document["outputs"], [])
                self.assertEqual(document["actions"], [{"name": "action"}] if "name:" in fields else [])

    def test_exactly_integral_float_fields_compile(self) -> None:
        source = """
        version: 1.0
        actions: [{name: action}]
        outputs:
          - type: led_fill
            action: action
            zone: {start: 0.0, length: 1.0, direction: start_to_end}
            color: {red: 255.0, green: 0.0, blue: 0.0}
            priority: 255.0
        """
        result, output = self.run_compiler(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(output)
        self.assertEqual(document["outputs"][0]["priority"], 255)
        self.assertEqual(document["outputs"][0]["zone"]["length"], 1)

    def test_explicit_null_lists_are_rejected(self) -> None:
        for field in ("actions", "rules", "outputs"):
            with self.subTest(field=field):
                result, output = self.run_compiler(f"version: 1\n{field}: null\n")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(field, result.stderr)
                self.assertIsNone(output)

    def test_nul_in_strings_is_rejected_without_writing_json(self) -> None:
        source = 'version: 1\nactions: [{name: "a\\0b"}]\n'
        result, output = self.run_compiler(source)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("actions[0].name", result.stderr)
        self.assertIn("NUL", result.stderr)
        self.assertIsNone(output)

    def test_invalid_timestamp_constructor_has_a_compile_diagnostic(self) -> None:
        source = "version: 1\nactions: [{name: 2026-02-31}]\nrules: []\noutputs: []\n"
        result, output = self.run_compiler(source)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("ERROR:", result.stderr)
        self.assertIn("YAML", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIsNone(output)

    def test_unsupported_schema_version_fails(self) -> None:
        source = """
        version: 2
        actions: []
        rules: []
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version", result.stderr)
        self.assertIn("1", result.stderr)
        self.assertIsNone(output_path)

    def test_duplicate_actions_fail(self) -> None:
        source = """
        version: 1
        actions:
          - name: duplicate
          - name: duplicate
        rules: []
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("actions[1].name", result.stderr)
        self.assertIn("unique", result.stderr)
        self.assertIsNone(output_path)

    def test_invalid_action_reference_fails(self) -> None:
        source = """
        version: 1
        actions:
          - name: declared
        rules:
          - type: state
            action: missing
            signal_key: vehicle.turn_state
            comparison: equal
            operand: {choice: left}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rules[0].action", result.stderr)
        self.assertIn("declared", result.stderr)
        self.assertIsNone(output_path)

    def test_unknown_signal_fails(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.does_not_exist
            comparison: equal
            operand: {choice: sideways}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown signal", result.stderr)
        self.assertIsNone(output_path)

    def test_signal_operand_type_mismatch_fails(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.turn_state
            comparison: equal
            operand: {number: 1}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("type", result.stderr)
        self.assertIsNone(output_path)

    def test_signal_capability_mismatch_fails(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.engine_rpm
            comparison: greater
            operand: {number: 6000}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("notify", result.stderr)
        self.assertIsNone(output_path)

    def test_unknown_signal_choice_fails(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.turn_state
            comparison: equal
            operand: {choice: sideways}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("choice", result.stderr)
        self.assertIsNone(output_path)

    def test_ambiguous_yaml_words_remain_enum_choices(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.turn_state
            comparison: equal
            value: off
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNotNone(output_path)
        self.assertEqual(json.loads(output_path.decode("utf-8"))["rules"][0]["operand"], {"choice": "off"})

    def test_hysteresis_matches_runtime_float_precision(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: sampled_state
            action: action
            signal_key: vehicle.engine_rpm
            comparison: greater
            operand: {number: 6000.0001}
            release_threshold: 6000.00001
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release_threshold", result.stderr)
        self.assertIsNone(output_path)

    def test_range_bounds_and_spans_match_runtime_float_precision(self) -> None:
        for lower, upper, accepted in (
            (-1.0e30, 3.4028234663852886e38, True),
            (-3.4028234663852886e38, 3.4028234663852886e38, False),
            (6000.00001, 6000.0001, False),
        ):
            with self.subTest(lower=lower, upper=upper):
                source = f"""
                version: 1
                actions: [{{name: fill}}]
                rules:
                  - type: range
                    action: fill
                    signal_key: vehicle.engine_rpm
                    input: {{from: {lower:.17e}, to: {upper:.17e}}}
                    output: {{from: 0, to: 1}}
                """
                result, output = self.run_compiler(source)
                self.assertEqual(result.returncode, 0 if accepted else 2, result.stderr)
                if not accepted:
                    self.assertIn("rules[0].input", result.stderr)
                    self.assertIsNone(output)

    def test_event_and_sampled_rules_compile_against_catalog(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: event
            action: action
            signal_key: vehicle.turn_state
            comparison: equal
            operand: {choice: left}
            edge: becomes_true
          - type: sampled_state
            action: action
            signal_key: vehicle.engine_rpm
            comparison: greater
            operand: {number: 6000}
            release_threshold: 5900
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNotNone(output_path)
        document = json.loads(output_path.decode("utf-8"))
        self.assertEqual(document["rules"][0]["type"], "event")
        self.assertEqual(document["rules"][1]["release_threshold"], 5900)

    def test_invalid_output_bounds_fail(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules: []
        outputs:
          - type: led_fill
            action: action
            zone: {start: 99, length: 2, direction: start_to_end}
            color: {red: 256, green: 0, blue: 0}
            priority: 256
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("priority", result.stderr)
        self.assertIsNone(output_path)

    def test_invalid_zone_bounds_fail(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules: []
        outputs:
          - type: led_fill
            action: action
            zone: {start: 99, length: 2, direction: start_to_end}
            color: {red: 0, green: 0, blue: 0}
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LED zone", result.stderr)
        self.assertIsNone(output_path)

    def test_invalid_color_bounds_fail(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules: []
        outputs:
          - type: led_fill
            action: action
            zone: {start: 0, length: 1, direction: start_to_end}
            color: {red: 256, green: 0, blue: 0}
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RGB channel", result.stderr)
        self.assertIsNone(output_path)

    def test_signal_catalog_manifest_covers_provider_keys(self) -> None:
        manifest = json.loads(SIGNAL_CATALOG.read_text(encoding="utf-8"))
        provider_source = SIGNAL_CATALOG_HEADER.read_text(encoding="utf-8")
        provider_keys = set(re.findall(r'"(vehicle\.[^"]+)"', provider_source))

        self.assertEqual(provider_keys, set(manifest["signals"]))

    def test_persisted_enum_names_have_compiler_spellings(self) -> None:
        from tools.compile_controller_config import ENUMS

        names_source = PERSISTED_NAMES.read_text(encoding="utf-8")
        tables = {
            enum: set(re.findall(r'"([a-z][a-z0-9_]*)"', body))
            for enum, body in re.findall(
                r"template <> struct Names<(\w+)> \{(.*?)\n\};", names_source, re.DOTALL
            )
        }
        expected = {
            "type": tables["RuleType"] | tables["OutputType"],
            "comparison": tables["Comparison"],
            "freshness": tables["FreshnessRequirement"],
            "edge": tables["EventEdge"],
            "effect": tables["LedEffect"],
            "direction": tables["FillDirection"],
        }
        self.assertEqual(set(ENUMS), set(expected))
        for kind, names in expected.items():
            with self.subTest(kind=kind):
                self.assertEqual(set(ENUMS[kind].values()), names)

    def test_deep_yaml_fails_with_a_compile_diagnostic(self) -> None:
        nested = "1"
        for _ in range(1000):
            nested = f"[{nested}]"
        source = f"version: 1\nactions: [{{name: action}}]\nrules: []\noutputs: {nested}\n"

        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nesting", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIsNone(output_path)

    def test_equivalent_yaml_emits_identical_bytes(self) -> None:
        first = """
        version: 1
        actions: [{name: rpm_fill}]
        rules:
          - type: range
            action: rpm_fill
            signal_key: vehicle.engine_rpm
            input: {from: 0, to: 6500.0}
            output: {from: 0.0, to: 1}
        outputs:
          - type: led_fill
            action: rpm_fill
            zone: {start: 0, length: 100, direction: center_out}
            color: {red: 0, green: 16, blue: 32}
        """
        second = """
        outputs:
          - priority: 100
            color: {blue: 32, green: 16, red: 0}
            zone: {direction: CenterOut, length: 100.0, start: 0.0}
            action: rpm_fill
            type: LedFill
        rules:
          - freshness: Fresh
            output: {to: 1.0, from: 0.0}
            input: {to: 6500, from: 0.0}
            signal_key: vehicle.engine_rpm
            action: rpm_fill
            type: Range
        actions: [{name: rpm_fill}]
        version: 1.0
        """

        first_result, first_path = self.run_compiler(first)
        second_result, second_path = self.run_compiler(second)

        self.assertEqual(first_result.returncode, 0, first_result.stderr)
        self.assertEqual(second_result.returncode, 0, second_result.stderr)
        self.assertIsNotNone(first_path)
        self.assertIsNotNone(second_path)
        self.assertEqual(first_path, second_path)
        self.assertEqual(first_path, first_path.rstrip() + b"\n")

    def test_duplicate_level_rules_fail(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules:
          - type: state
            action: action
            signal_key: vehicle.turn_state
            comparison: equal
            operand: {choice: left}
          - type: sampled_state
            action: action
            signal_key: vehicle.engine_rpm
            comparison: greater
            operand: {number: 6000}
        outputs: []
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rules[1].action", result.stderr)
        self.assertIsNone(output_path)

    def test_duplicate_output_binding_fails(self) -> None:
        source = """
        version: 1
        actions: [{name: action}]
        rules: []
        outputs:
          - type: led_effect
            action: action
            effect: brake
          - type: LedEffect
            action: action
            effect: Brake
        """
        result, output_path = self.run_compiler(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outputs[1]", result.stderr)
        self.assertIsNone(output_path)

    def test_production_yaml_matches_canonical_json_example(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "controller-config.json"
            result = subprocess.run(
                [sys.executable, str(COMPILER), str(PRODUCTION_YAML), str(output_path)],
                cwd=REPOSITORY,
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(output_path.read_text(encoding="utf-8")),
                json.loads(PRODUCTION_JSON.read_text(encoding="utf-8")),
            )


if __name__ == "__main__":
    unittest.main()
