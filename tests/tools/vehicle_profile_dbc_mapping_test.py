"""Mapping-file validation: enum/boolean targets, choices, locations and golden vectors."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests/tools"))

from vehicle_profile_dbc_import_test import DEMO_A, MAPPING, codes, staged
from vehicle_profiles.dbc_import import import_profile
from vehicle_profiles.dbc_mapping import (MappingError, load_mapping, locate, mapping_revision,
                                           semantic_revision)

BODY = """BO_ 512 Body: 8 ECU
 SG_ Gear : 0|3@1+ (1,0) [0|7] "" Controller
 SG_ Flag : 3|1@1+ (1,0) [0|1] "" Controller
 SG_ FlagU : 4|1@1+ (1,0) [0|1] "x" Controller
 SG_ Wide : 8|8@1+ (1,0) [0|255] "" Controller
 SG_ S : 5|1@1- (1,0) [-1|0] "" Controller
 SG_ R : 6|1@1+ (1,-1) [-1|0] "" Controller
 SG_ Z : 7|1@1+ (1,0) [0|0] "" Controller
VAL_ 512 Gear 0 "Unk" 1 "Park" 2 "Back" 3 "Idle" 4 "Go" 5 "Go" ;
"""
DBC = DEMO_A + BODY
GEAR = {"target": "vehicle.selector_position", "message": "Body", "field": "Gear", "unit": "",
        "choices": {"Park": "park", "Back": "reverse", "Go": "drive"}}
FLAG = {"target": "vehicle.hazard_request", "message": "Body", "field": "Flag", "unit": ""}


def run(*bindings, mutate=None):
    def edit(document):
        document["bindings"].extend(json.loads(json.dumps(list(bindings))))
        if mutate:
            mutate(document)
    return import_profile(staged(edit, dbc=DBC))


def item(result, code):
    (found,) = [i for i in result.report["diagnostics"] if i["code"] == code]
    return found


def bound(result, key):
    return [b for b in result.profile["bindings"] if b["key"] == key][0]


class EnumAndBooleanTargets(unittest.TestCase):
    def test_enum_binding_maps_value_table_labels_to_catalog_choices(self):
        binding = bound(run(GEAR), "vehicle.selector_position")
        self.assertEqual(binding["type"], "enum")
        self.assertEqual(binding["unit"], "")
        self.assertEqual([c["key"] for c in binding["choices"]],
                         ["unknown", "shifting", "park", "reverse", "neutral", "drive"])
        self.assertEqual([c["value"] for c in binding["choices"]], [0, 1, 2, 3, 4, 5])
        self.assertEqual(binding["policy"]["enum_map"], [
            {"raw": "1", "choice": "park"}, {"raw": "2", "choice": "reverse"},
            {"raw": "4", "choice": "drive"}, {"raw": "5", "choice": "drive"}])
        self.assertEqual(binding["policy"]["conversion"]["target_precision"], "uint16")

    def test_boolean_binding_from_a_one_bit_zero_one_field(self):
        binding = bound(run(FLAG), "vehicle.hazard_request")
        self.assertEqual((binding["type"], binding["unit"], binding["choices"]),
                         ("boolean", "", []))
        self.assertEqual(binding["policy"]["enum_map"], [])
        self.assertEqual(binding["policy"]["conversion"]["target_precision"], "boolean")

    def test_unit_bearing_field_needs_an_explicit_identity_for_boolean_target(self):
        flag = dict(FLAG, field="FlagU")
        self.assertIn("incompatible_target", codes(run(flag)))
        ok = run(dict(flag, conversion={"scale": "1.0", "offset": "0"}))
        self.assertIsNotNone(ok.profile)
        self.assertEqual(
            bound(ok, "vehicle.hazard_request")["policy"]["conversion"]["source_unit"], "x")

    def test_incompatible_matrix(self):
        cases = {
            "unknown label": dict(GEAR, choices={"Nope": "park"}),
            "unknown choice key": dict(GEAR, choices={"Park": "sideways"}),
            "enum without choices": {k: v for k, v in GEAR.items() if k != "choices"},
            "choices on a number target": dict(GEAR, target="vehicle.speed_kph"),
            "choices on a boolean target": dict(FLAG, choices={"Park": "park"}),
            "enum target with unit": dict(GEAR, unit="rpm"),
            "boolean target with unit": dict(FLAG, unit="rpm"),
            "boolean from a multi-bit field": dict(FLAG, field="Wide"),
            "boolean from a signed one-bit field": dict(FLAG, field="S"),
            "boolean from a one-bit field with range -1..0": dict(FLAG, field="R"),
            "boolean from a one-bit field with range 0..0": dict(FLAG, field="Z"),
            "boolean from a number field": dict(FLAG, message="Engine", field="rpm"),
            "non-identity conversion on enum": dict(GEAR, conversion={"scale": "2", "offset": "0"}),
            "non-identity conversion on boolean": dict(FLAG, conversion={"scale": "1", "offset": "1"}),
        }
        for name, binding in cases.items():
            result = run(binding)
            self.assertIsNone(result.profile, name)
            self.assertIn("incompatible_target", codes(result), name)
            self.assertEqual({i["stage"] for i in result.report["diagnostics"]}, {"mapping"}, name)

    def test_brake_pressure_to_brake_pressed_is_never_coerced(self):
        result = run({"target": "vehicle.brake_pressed", "message": "Engine",
                      "field": "BrakePressure", "unit": ""})
        self.assertIsNone(result.profile)
        self.assertIn("incompatible_target", codes(result))

    def test_choices_must_be_a_nonempty_string_map(self):
        base = json.loads(MAPPING.read_text())
        for bad in ({}, [], {"Park": 1}, {"": "park"}, {"Park": ""}, "park", None):
            document = json.loads(json.dumps(base))
            document["bindings"][0]["choices"] = bad
            with self.assertRaises(MappingError, msg=repr(bad)):
                load_mapping(json.dumps(document))


class Locations(unittest.TestCase):
    def test_locate_reports_one_based_spans(self):
        text = '{\n  "a": [1,\n   {"b": "x"}]\n}'
        spans = locate(text)
        self.assertEqual(spans["$"], (1, 1, 4, 2))
        self.assertEqual(spans["$.a"], (2, 8, 3, 15))
        self.assertEqual(spans["$.a[1].b"], (3, 10, 3, 13))

    def test_mapping_errors_carry_json_location(self):
        document = json.loads(MAPPING.read_text())
        document["bindings"][0]["conversion"] = {"scale": "1", "offset": "0x10"}
        text = json.dumps(document, indent=1)
        with self.assertRaises(MappingError) as caught:
            load_mapping(text)
        location = caught.exception.location
        self.assertEqual(location["path"], "$.bindings[0].conversion.offset")
        self.assertIn('"offset": "0x10"', text.splitlines()[location["line"] - 1])

    def test_syntax_error_has_line_and_column(self):
        with self.assertRaises(MappingError) as caught:
            load_mapping('{\n "a": ,}')
        self.assertEqual(caught.exception.location["line"], 2)

    def test_report_diagnostics_carry_the_mapping_location(self):
        cases = (
            ("unknown_target", dict(FLAG, target="vehicle.nope"), "$.bindings[1].target"),
            ("stale_mapping", dict(FLAG, field="Gone"), "$.bindings[1].field"),
            ("stale_mapping", dict(FLAG, message="Gone"), "$.bindings[1].message"),
            ("incompatible_target", dict(GEAR, unit="rpm"), "$.bindings[1].unit"),
            ("incompatible_target", dict(GEAR, choices={"Nope": "park"}), "$.bindings[1].choices"),
            ("incompatible_target", dict(FLAG, field="Wide"), "$.bindings[1]"),
        )
        for code, binding, path in cases:
            result = run(binding)
            mapped = [l for l in item(result, code)["locations"] if l.get("path")]
            self.assertEqual([l["path"] for l in mapped], [path], code)
            self.assertEqual(mapped[0]["source_id"], "mapping.json")
            self.assertGreaterEqual(mapped[0]["line"], 1)

    def test_duplicate_target_points_at_the_second_binding(self):
        result = run(FLAG, dict(FLAG, field="FlagU"))
        mapped = [l for l in item(result, "duplicate_target")["locations"] if l.get("path")]
        self.assertEqual(mapped[0]["path"], "$.bindings[2].target")

    def test_conflicting_conversion_lists_each_binding_location(self):
        kpa = {"message": "Engine", "field": "BrakePressure", "unit": "bar"}
        result = run(dict(kpa, target="vehicle.brake_pressure",
                          conversion={"scale": "0.01", "offset": "0"}),
                     dict(kpa, target="vehicle.acceleration.longitudinal",
                          conversion={"scale": "0.02", "offset": "0"}))
        paths = [l["path"] for l in item(result, "conflicting_conversion")["locations"]
                 if l.get("path")]
        # Listed in sorted target order: acceleration (index 2) before brake_pressure (index 1).
        self.assertEqual(paths, ["$.bindings[2]", "$.bindings[1]"])

    def test_invalid_freshness_is_a_report_diagnostic_not_an_exception(self):
        for value in (0, -5, True, "9", 4294967296, 1.5):
            result = import_profile(staged(lambda d: d["bindings"][0].update(timeout_ms=value)))
            self.assertIsNone(result.profile, value)
            self.assertIsNone(result.identity)
            found = item(result, "invalid_freshness")
            self.assertEqual(found["severity"], "error")
            self.assertEqual(found["stage"], "mapping")
            self.assertEqual(found["affected"], "vehicle.engine_rpm")
            (location,) = found["locations"]
            self.assertEqual(location["path"], "$.bindings[0].timeout_ms")
            self.assertEqual(set(result.report), {"profile_id", "semantic_revision",
                                                  "mapping_revision", "sources", "freshness",
                                                  "diagnostics"})

    def test_zero_scale_is_a_plain_mapping_file_error(self):
        document = json.loads(MAPPING.read_text())
        document["bindings"][0]["conversion"] = {"scale": "0", "offset": "0"}
        with self.assertRaises(MappingError) as caught:
            load_mapping(json.dumps(document))
        self.assertEqual(caught.exception.code, "invalid_mapping")
        self.assertEqual(caught.exception.location["path"], "$.bindings[0].conversion.scale")


class MultiSourceLocations(unittest.TestCase):
    def test_mapping_locations_and_alias_names_work_with_two_sources(self):
        from vehicle_profile_dbc_compose_test import A, B, ALIAS, run as run_two
        bad = dict(ALIAS, target="vehicle.nope")
        result = run_two({"demo-a": A, "demo-b": B}, bindings=[ALIAS, bad])
        found = item(result, "unknown_target")
        self.assertEqual(found["source_ids"], ["demo-a", "demo-b"])
        mapped = [l for l in found["locations"] if l.get("path")]
        self.assertEqual([l["path"] for l in mapped], ["$.bindings[1].target"])
        stale = run_two({"demo-a": A, "demo-b": B}, bindings=[dict(ALIAS, field="Gone")])
        self.assertEqual([l["path"] for l in item(stale, "stale_mapping")["locations"]
                          if l.get("path")], ["$.bindings[0].field"])
        timeout = run_two({"demo-a": A, "demo-b": B},
                          bindings=[dict(ALIAS, timeout_ms=0)])
        self.assertEqual(item(timeout, "invalid_freshness")["source_ids"], ["demo-a", "demo-b"])


class ProfileDirectory(unittest.TestCase):
    def stage(self, make, model, profile_id):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        directory = Path(tmp.name, "vehicle-profiles", make, model)
        directory.mkdir(parents=True)
        (directory / "demo-a.dbc").write_text(DEMO_A)
        document = json.loads(MAPPING.read_text())
        document["profile_id"] = profile_id
        (directory / "mapping.json").write_text(json.dumps(document, indent=1))
        return directory / "mapping.json"

    def test_profile_id_matches_the_make_model_directory(self):
        self.assertIsNotNone(import_profile(self.stage("acme", "road", "acme.road")).profile)

    def test_mismatched_profile_id_under_vehicle_profiles_is_rejected(self):
        with self.assertRaises(MappingError) as caught:
            import_profile(self.stage("acme", "road", "acme.other"))
        self.assertEqual(caught.exception.code, "invalid_mapping")
        self.assertEqual(caught.exception.location["path"], "$.profile_id")

    def test_other_directories_are_unconstrained(self):
        self.assertIsNotNone(import_profile(staged()).profile)


class GoldenVectors(unittest.TestCase):
    PREIMAGE = (
        '{"bindings":[{"choices":{"Back":"reverse","Go":"drive","Park":"park"},'
        '"field":"Gear","message":"Body","target":"vehicle.selector_position","unit":""},'
        '{"conversion":{"offset":"-40","scale":"0.1"},"field":"T","message":"Body",'
        '"target":"vehicle.speed_kph","timeout_ms":250,"unit":"degC"}],'
        '"profile_id":"synthetic.demo","selected_bus":"body","version":1}')

    WHOLE_PREIMAGE = PREIMAGE[:-len('"version":1}')] + (
        '"sources":[{"bus":"body","encoding":"utf-8","license":"Apache-2.0; synthetic fixture",'
        '"path":"demo-a.dbc","sha256":'
        '"b884d194c8f876930fac7dbb0834e2fabb1f727931a31f0ae677db6edc665b88"}],"version":1}')

    def mapping(self, reverse=False):
        document = json.loads(MAPPING.read_text())
        document["bindings"] = [
            {"unit": "degC", "timeout_ms": 250, "field": "T", "message": "Body",
             "target": "vehicle.speed_kph",
             "conversion": {"scale": "1e-1", "offset": "-40.0"}},
            {"unit": "", "target": "vehicle.selector_position", "message": "Body",
             "field": "Gear", "choices": {"Park": "park", "Go": "drive", "Back": "reverse"}}]
        if reverse:
            document["bindings"].reverse()
            document["bindings"][0]["choices"] = dict(sorted(
                document["bindings"][0]["choices"].items()))
        return load_mapping(json.dumps(document))

    def test_semantic_revision_pins_conversion_and_choices_normalization(self):
        digest = "mapping-v1:" + hashlib.sha256(self.PREIMAGE.encode()).hexdigest()
        for reverse in (False, True):
            self.assertEqual(semantic_revision(self.mapping(reverse)), digest)
        self.assertEqual(digest, GOLDEN_SEMANTIC)

    def test_mapping_revision_adds_sources_and_a_choices_edit_changes_both(self):
        base = self.mapping()
        self.assertEqual(mapping_revision(base), GOLDEN_WHOLE)
        self.assertEqual(mapping_revision(self.mapping(reverse=True)), GOLDEN_WHOLE)
        self.assertEqual(GOLDEN_WHOLE, "mapping-v1:"
                         + hashlib.sha256(self.WHOLE_PREIMAGE.encode()).hexdigest())
        edited = self.mapping()
        edited["bindings"][1]["choices"]["Go"] = "neutral"
        self.assertNotEqual(semantic_revision(edited), semantic_revision(base))
        self.assertNotEqual(mapping_revision(edited), mapping_revision(base))

    def test_choices_edit_changes_compatibility_identity(self):
        base = run(GEAR)
        other = run(dict(GEAR, choices={"Park": "park", "Back": "reverse"}))
        self.assertNotEqual(base.profile["compatibility_identity"],
                            other.profile["compatibility_identity"])
        relabelled = run(GEAR, mutate=lambda d: d["sources"][0].update(license="other"))
        self.assertEqual(base.profile["compatibility_identity"],
                         relabelled.profile["compatibility_identity"])


GOLDEN_SEMANTIC = "mapping-v1:aba565bbeff67dc66c7055430f6d30316f2494ddf62e5adfe46e15fa8a329eb1"
GOLDEN_WHOLE = "mapping-v1:6dee7254aa47f9daa384a22e7086a892d8f457d8301bc79b017ae558c5ce3170"

if __name__ == "__main__":
    unittest.main()
