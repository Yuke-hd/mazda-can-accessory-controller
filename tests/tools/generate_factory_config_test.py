"""Tests for profile-aware filtering of the built-in factory features."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import unittest

from tools.compile_controller_config import load_yaml
from tools.generate_factory_config import filter_factory_document


REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_YAML = REPOSITORY / "config" / "default.yaml"


def signal(signal_type: str, capabilities: tuple[str, ...], choices: tuple[str, ...] = ()):
    return {"type": signal_type, "capabilities": frozenset(capabilities), "choices": choices}


def profile(*bindings: tuple[str, bool]):
    return {"bindings": [
        {"key": key, "freshness": {"timeout_ms": 100} if fresh else None}
        for key, fresh in bindings
    ]}


class FactoryFeatureFilterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = load_yaml(DEFAULT_YAML)

    def filter(self, catalog, vehicle_profile):
        return filter_factory_document(
            deepcopy(self.document), catalog, vehicle_profile, native=False
        )

    def test_native_factory_document_is_unchanged(self) -> None:
        result, diagnostics = filter_factory_document(
            deepcopy(self.document), {}, None, native=True
        )
        self.assertEqual(result, self.document)
        self.assertEqual(diagnostics, [])

    def test_partial_import_keeps_only_supported_complete_features(self) -> None:
        catalog = {
            "vehicle.turn_state": signal("enum", ("notify",), ("left", "right")),
        }
        result, diagnostics = self.filter(
            catalog, profile(("vehicle.turn_state", True))
        )

        self.assertEqual([action["name"] for action in result["actions"]],
                         ["left_turn", "right_turn"])
        self.assertEqual([rule["action"] for rule in result["rules"]],
                         ["left_turn", "right_turn"])
        self.assertEqual([output["action"] for output in result["outputs"]],
                         ["left_turn", "right_turn"])
        self.assertTrue(any("missing choice 'hazard'" in item for item in diagnostics))
        self.assertTrue(any("missing input signal 'vehicle.engine_rpm'" in item for item in diagnostics))
        self.assertTrue(any("missing input signal 'vehicle.brake_pressed'" in item for item in diagnostics))

    def test_imported_features_require_declared_timeout_and_fresh_rules(self) -> None:
        catalog = {
            "vehicle.engine_rpm": signal("number", ("read",)),
        }
        result, diagnostics = self.filter(
            catalog, profile(("vehicle.engine_rpm", True))
        )

        self.assertEqual([action["name"] for action in result["actions"]],
                         ["rpm_fill", "red_zone"])
        self.assertTrue(all(rule["freshness"] == "fresh" for rule in result["rules"]))
        self.assertTrue(all(output["action"] in {"rpm_fill", "red_zone"}
                            for output in result["outputs"]))
        self.assertTrue(all("fresh_or_unverified" not in str(rule) for rule in result["rules"]))
        self.assertTrue(any("vehicle.turn_state" in item for item in diagnostics))

    def test_imported_signal_without_timeout_keeps_catalog_but_disables_feature(self) -> None:
        catalog = {"vehicle.engine_rpm": signal("number", ("read",))}
        result, diagnostics = self.filter(
            catalog, profile(("vehicle.engine_rpm", False))
        )

        self.assertEqual(result["actions"], [])
        self.assertEqual(result["rules"], [])
        self.assertEqual(result["outputs"], [])
        self.assertTrue(any("no declared Fresh timeout" in item for item in diagnostics))

    def test_type_capability_and_missing_inputs_have_actionable_diagnostics(self) -> None:
        catalog = {
            "vehicle.turn_state": signal("number", ("read",)),
            "vehicle.engine_rpm": signal("boolean", ("notify",)),
        }
        _, diagnostics = self.filter(
            catalog,
            profile(("vehicle.turn_state", True), ("vehicle.engine_rpm", True)),
        )

        self.assertTrue(any("vehicle.turn_state" in item and "requires enum" in item
                            for item in diagnostics))
        self.assertTrue(any("vehicle.engine_rpm" in item and "requires number" in item
                            for item in diagnostics))
        self.assertTrue(any("vehicle.brake_pressed" in item and "missing input" in item
                            for item in diagnostics))

        catalog["vehicle.turn_state"] = signal(
            "enum", ("read",), ("left", "right", "hazard")
        )
        _, diagnostics = self.filter(
            catalog, profile(("vehicle.turn_state", True), ("vehicle.engine_rpm", True))
        )
        self.assertTrue(any("vehicle.turn_state" in item and "missing required 'notify' capability" in item
                            for item in diagnostics))

    def test_empty_profile_emits_empty_features_and_preserves_sink_settings(self) -> None:
        document = deepcopy(self.document)
        document["sinks"] = {"local_argb": {"pixel_count": 100}}
        result, _ = filter_factory_document(document, {}, {"bindings": []}, native=False)

        self.assertEqual(result["actions"], [])
        self.assertEqual(result["rules"], [])
        self.assertEqual(result["outputs"], [])
        self.assertEqual(result["sinks"], document["sinks"])


if __name__ == "__main__":
    unittest.main()
