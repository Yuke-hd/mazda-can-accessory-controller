"""Independent structural contract checks; no DBC/runtime support is claimed."""

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from vehicle_profiles.validation import ProfileValidationError, load_profile, validate_structure

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles"


class ProfileStructureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.profile = json.loads((FIXTURES / "synthetic-river.json").read_text())

    def assert_invalid(self, profile, category, path=None):
        with self.assertRaises(ProfileValidationError) as caught:
            validate_structure(profile)
        self.assertEqual(caught.exception.category, category)
        if path is not None:
            self.assertEqual(caught.exception.path, path)
        return caught.exception

    def test_independent_populated_and_empty_profiles(self):
        populated = load_profile((FIXTURES / "synthetic-river.json").read_text())
        empty = load_profile((FIXTURES / "synthetic-empty.json").read_text())
        self.assertEqual(len(populated["sources"]), 2)
        self.assertEqual(populated["bindings"][0]["kind"], "dbc")
        self.assertEqual(populated["bindings"][1]["kind"], "native")
        self.assertEqual(empty["bindings"], [])
        self.assertNotEqual(empty["selected_bus"], populated["selected_bus"])

    def test_numeric_strings_retain_exact_lexemes(self):
        field = validate_structure(self.profile)["messages"][0]["fields"][0]
        self.assertEqual(field["scale"], "0.0100000000000000000001")
        self.assertEqual(field["maximum"], "655.3500000000000000065535")
        wide = self.profile["messages"][0]["fields"][1]
        wide["length"] = 64
        wide["maximum"] = "18446744073709551615"
        wide["value_table"][0]["raw"] = "18446744073709551615"
        self.assertEqual(validate_structure(self.profile)["messages"][0]["fields"][1]["value_table"][0]["raw"], "18446744073709551615")

    def test_duplicate_keys_at_any_depth_are_rejected(self):
        for text in ('{"format":"a","format":"b"}', '{"nested":{"id":1,"id":2}}'):
            with self.subTest(text=text), self.assertRaises(ProfileValidationError) as caught:
                load_profile(text)
            self.assertEqual(caught.exception.category, "duplicate_key")

    def test_missing_unknown_and_wrongly_typed_fields(self):
        for mutate, category in ((lambda p: p.pop("messages"), "missing_field"),
                                 (lambda p: p.update(extra=1), "unknown_field"),
                                 (lambda p: p.update(bindings={}), "invalid_type"),
                                 (lambda p: p.update(version=True), "invalid_type"),
                                 (lambda p: p.update(version=2), "unsupported_version"),
                                 (lambda p: p.update(profile_id="bad id"), "invalid_identity")):
            with self.subTest(category=category):
                profile = copy.deepcopy(self.profile)
                mutate(profile)
                self.assert_invalid(profile, category)

    def test_unknown_nested_field_and_native_dbc_variants(self):
        self.profile["bindings"][1]["policy"]["conversion"] = {}
        self.assert_invalid(self.profile, "unknown_field", "$.bindings[1].policy")

    def test_duplicate_and_unknown_references(self):
        for mutate in (lambda p: p["sources"].append(copy.deepcopy(p["sources"][0])),
                       lambda p: p["bindings"][1].update(runtime_id=1),
                       lambda p: p["messages"][0]["fields"].append(copy.deepcopy(p["messages"][0]["fields"][0]))):
            profile = copy.deepcopy(self.profile)
            mutate(profile)
            self.assert_invalid(profile, "duplicate_identity")
        self.profile["bindings"][0]["inputs"] = ["not.a.field"]
        self.assert_invalid(self.profile, "invalid_reference", "$.bindings[0].inputs[0]")

    def test_source_diagnostics_retain_source_identity(self):
        self.profile["messages"][0]["fields"][0]["scale"] = 0.01
        error = self.assert_invalid(self.profile, "invalid_type")
        self.assertEqual(error.sources, ("river-a", "river-b"))

    def test_absolute_or_traversing_paths_are_rejected(self):
        for path in ("/tmp/source.dbc", "../source.dbc", "C:/source.dbc", "folder\\source.dbc"):
            with self.subTest(path=path):
                self.profile["sources"][0]["path"] = path
                self.assert_invalid(self.profile, "invalid_source_path")

    def test_nonfinite_json_and_invalid_numeric_strings(self):
        with self.assertRaises(ProfileValidationError) as caught:
            load_profile('{"bad":NaN}')
        self.assertEqual(caught.exception.category, "malformed_json")
        for value in ("NaN", "Infinity", "01", "0x12", ""):
            with self.subTest(value=value):
                self.profile["messages"][0]["fields"][0]["scale"] = value
                self.assert_invalid(self.profile, "invalid_numeric_lexeme" if value else "invalid_value")

    def test_empty_profile_requires_explicit_disabled_report(self):
        self.profile["bindings"] = []
        self.assert_invalid(self.profile, "missing_empty_profile_diagnostic", "$.diagnostics")

    def test_enum_map_references_existing_choice_keys(self):
        binding = self.profile["bindings"][0]
        binding["type"] = "enum"
        binding["choices"] = [{"key": "moving", "value": 1}]
        binding["policy"]["enum_map"] = [{"raw": "1", "choice": "stopped"}]
        self.assert_invalid(self.profile, "invalid_reference", "$.bindings[0].policy.enum_map[0].choice")

    def test_explicit_invalid_range_shape_and_lexemes(self):
        conversion = self.profile["bindings"][0]["policy"]["conversion"]
        self.assertEqual(conversion["invalid_ranges"], [{"minimum": "655.3400000000000000065534", "maximum": "655.3500000000000000065535"}])
        result = validate_structure(self.profile)
        self.assertEqual(result["bindings"][0]["policy"]["conversion"]["invalid_ranges"], conversion["invalid_ranges"])
        conversion["invalid_ranges"][0]["minimum"] = 655.34
        self.assert_invalid(self.profile, "invalid_type", "$.bindings[0].policy.conversion.invalid_ranges[0].minimum")
        conversion["invalid_ranges"] = None
        self.assert_invalid(self.profile, "invalid_type", "$.bindings[0].policy.conversion.invalid_ranges")


if __name__ == "__main__":
    unittest.main()
