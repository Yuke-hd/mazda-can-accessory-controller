"""Tests for the host-side YAML controller configuration compiler."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
COMPILER = REPOSITORY / "tools" / "compile_controller_config.py"
PRODUCTION_YAML = REPOSITORY / "docs" / "examples" / "controller-config-v1.yaml"
PRODUCTION_JSON = REPOSITORY / "docs" / "examples" / "controller-config-v1.json"


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
            signal_key: vehicle.a
            comparison: equal
            operand: {boolean: true}
          - type: sampled_state
            action: action
            signal_key: vehicle.b
            comparison: equal
            operand: {boolean: true}
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
