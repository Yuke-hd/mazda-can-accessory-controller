"""Multi-source DBC composition through mapping.json: dedupe, nonoverlapping merge, source_conflict."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.dbc_import import import_profile
from vehicle_profiles.dbc_mapping import MappingError

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
A = (FIXTURES / "demo-a.dbc").read_text()
B = (FIXTURES / "demo-b.dbc").read_text()
RPM = {"target": "vehicle.engine_rpm", "message": "Engine", "field": "rpm", "unit": "rpm"}
ALIAS = dict(RPM, message="EngineExtra", field="engine_rpm")
KEEP = []


def run(texts, order=None, buses=None, bindings=None, mutate=None):
    """Import `texts` (source id -> DBC text); `order` is the mapping's source order."""
    tmp = tempfile.TemporaryDirectory()
    KEEP.append(tmp)
    root = Path(tmp.name)
    sources = []
    for sid in order or list(texts):
        (root / (sid + ".dbc")).write_text(texts[sid])
        sources.append({"path": sid + ".dbc", "sha256": hashlib.sha256(
            texts[sid].encode()).hexdigest(), "bus": (buses or {}).get(sid, "body"),
            "license": "Apache-2.0; synthetic fixture", "encoding": "utf-8"})
    document = {"version": 1, "profile_id": "synthetic.demo", "selected_bus": "body",
                "sources": sources, "bindings": [RPM] if bindings is None else bindings}
    if mutate:
        mutate(document)
    (root / "mapping.json").write_text(json.dumps(document))
    return import_profile(root / "mapping.json")


def dump(result):
    return json.dumps([result.profile, result.report], sort_keys=True)


class ComposeTests(unittest.TestCase):
    def test_demo_a_and_b_bind_rpm_with_both_sources_and_leave_others_unmapped(self):
        result = run({"demo-a": A, "demo-b": B})
        (message,) = result.profile["messages"]
        by_name = {f["name"]: f for f in message["fields"]}
        self.assertEqual(len(message["fields"]), 3)
        (binding,) = result.profile["bindings"]
        self.assertEqual(binding["key"], "vehicle.engine_rpm")
        rpm = next(f for f in message["fields"] if f["id"] == binding["inputs"][0])
        self.assertEqual(rpm["sources"], ["demo-a", "demo-b"])
        unmapped = {d["affected"] for d in result.profile["diagnostics"]
                    if d["category"] == "unmapped"}
        self.assertEqual(unmapped, {by_name["CoolantRaw"]["id"], by_name["BrakePressure"]["id"]})
        self.assertEqual([s["id"] for s in result.profile["sources"]], ["demo-a", "demo-b"])

    def test_binding_may_use_any_alias_name(self):
        by_original = run({"demo-a": A, "demo-b": B})
        by_alias = run({"demo-a": A, "demo-b": B}, bindings=[ALIAS])
        self.assertEqual(by_alias.profile["bindings"][0]["inputs"],
                         by_original.profile["bindings"][0]["inputs"])

    def test_reversed_source_order_is_byte_identical(self):
        first = run({"demo-a": A, "demo-b": B}, ["demo-a", "demo-b"])
        second = run({"demo-a": A, "demo-b": B}, ["demo-b", "demo-a"])
        self.assertEqual(dump(first), dump(second))
        self.assertEqual(first.identity.artifact_preimage, second.identity.artifact_preimage)
        self.assertEqual(first.identity.compatibility_preimage,
                         second.identity.compatibility_preimage)

    def test_alias_inventory_keeps_every_original_name_and_location(self):
        entries = run({"demo-a": A, "demo-b": B}).report["aliases"]
        rpm = next(e for e in entries if e["kind"] == "field" and
                   {a["name"] for a in e["aliases"]} == {"rpm", "engine_rpm"})
        self.assertEqual({a["source_id"] for a in rpm["aliases"]}, {"demo-a", "demo-b"})
        self.assertTrue(all(a["locations"] for a in rpm["aliases"]))
        message = next(e for e in entries if e["kind"] == "message")
        self.assertEqual({a["name"] for a in message["aliases"]}, {"Engine", "EngineExtra"})

    def test_repeated_identical_message_in_one_source_deduplicates(self):
        repeat = 'BO_ 256 Engine2: 8 ECU\n SG_ rpm : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Controller\n'
        (message,) = run({"demo-a": A + repeat}).profile["messages"]
        self.assertEqual(len(message["fields"]), 2)

    def test_single_source_report_has_alias_inventory(self):
        fields = [e for e in run({"demo-a": A}).report["aliases"] if e["kind"] == "field"]
        self.assertEqual(len(fields), 2)

    def test_same_name_redefinition_in_repeated_message_fails(self):
        repeat = 'BO_ 256 Engine2: 8 ECU\n SG_ rpm : 0|8@1+ (1,0) [0|255] "rpm" Controller\n'
        result = run({"demo-a": A + repeat})
        self.assertIsNone(result.profile)
        self.assertEqual(result.report["diagnostics"][0]["code"], "source_conflict")


class SelectedBusTests(unittest.TestCase):
    def test_other_bus_sources_stay_inventory_and_do_not_compose(self):
        changed = B.replace("(0.250,0.0)", "(0.5,0)")  # would conflict if composed
        result = run({"demo-a": A, "demo-b": changed}, buses={"demo-b": "chassis"})
        self.assertIsNotNone(result.profile)
        self.assertEqual([s["id"] for s in result.profile["sources"]], ["demo-a"])
        self.assertEqual([s["id"] for s in result.report["sources"]], ["demo-a", "demo-b"])
        (message,) = result.profile["messages"]
        self.assertEqual(len(message["fields"]), 2)

    def test_binding_to_a_field_only_on_another_bus_is_stale(self):
        result = run({"demo-a": A, "demo-b": B}, buses={"demo-b": "chassis"}, bindings=[ALIAS])
        self.assertIsNone(result.profile)
        self.assertEqual(result.report["diagnostics"][0]["code"], "stale_mapping")

    def test_no_source_on_the_selected_bus_is_a_mapping_error(self):
        with self.assertRaises(MappingError):
            run({"demo-a": A}, buses={"demo-a": "chassis"})

    def test_duplicate_source_ids_are_a_mapping_error(self):
        def duplicate(document):
            document["sources"][1]["path"] = "sub/demo-a.dbc"
        with self.assertRaises(MappingError):
            run({"demo-a": A, "demo-b": B}, mutate=duplicate)


class ConflictTests(unittest.TestCase):
    def conflict(self, b_text, a_text=A, reason_part="", bindings=None):
        for order in (["demo-a", "demo-b"], ["demo-b", "demo-a"]):
            result = run({"demo-a": a_text, "demo-b": b_text}, order, bindings=bindings)
            self.assertIsNone(result.profile)
            self.assertIsNone(result.identity)
            (item,) = result.report["diagnostics"]
            self.assertEqual((item["code"], item["severity"]), ("source_conflict", "error"))
            self.assertEqual(item["source_ids"], ["demo-a", "demo-b"])
            self.assertEqual({l["source_id"] for l in item["locations"]}, {"demo-a", "demo-b"})
            self.assertIn(reason_part, item["reason"])

    def test_changed_duplicate_scale(self):
        self.conflict(B.replace("(0.250,0.0)", "(0.5,0)"))

    def test_changed_duplicate_offset(self):
        self.conflict(B.replace("(0.250,0.0)", "(0.25,1)"))

    def test_changed_duplicate_range(self):
        self.conflict(B.replace("[0|16383.750]", "[0|100]"))

    def test_changed_duplicate_range_minimum(self):
        self.conflict(B.replace("[0|16383.750]", "[-1|16383.750]"))

    def test_changed_duplicate_signedness(self):
        self.conflict(B.replace("0|16@1+", "0|16@1-"))

    def test_changed_duplicate_unit(self):
        self.conflict(B.replace('"rpm"', '"RPM"'))

    def test_changed_dlc(self):
        self.conflict(B.replace("EngineExtra: 8", "EngineExtra: 7"), reason_part="payload length")

    def test_partial_overlap(self):
        self.conflict(B.replace("engine_rpm : 0|16", "engine_rpm : 8|16"), reason_part="overlap")

    def test_conflicting_raw_labels(self):
        self.conflict(B + 'VAL_ 256 engine_rpm 1 "Run" ;\n', a_text=A + 'VAL_ 256 rpm 1 "Idle" ;\n',
                      reason_part="label")

    def test_same_signal_name_with_different_mechanics_in_one_message(self):
        self.conflict(B.replace("CoolantRaw : 32|8", "rpm : 32|8"), reason_part="name")

    def test_contradiction_in_a_message_no_binding_uses_still_fails(self):
        a = A + 'BO_ 512 Other: 8 ECU\n SG_ Foo : 0|8@1+ (1,0) [0|255] "" Controller\n'
        b = B + 'BO_ 512 Other: 8 ECU\n SG_ Foo : 0|8@1+ (2,0) [0|255] "" Controller\n'
        self.conflict(b, a_text=a)
        self.conflict(b, a_text=a, bindings=[])

    def test_contradiction_in_an_unsupported_message_still_fails(self):
        a = A + 'BO_ 2147483905 Wide: 8 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n'
        b = B + 'BO_ 2147483905 Wide: 8 ECU\n SG_ X : 0|8@1+ (3,0) [0|255] "" Controller\n'
        self.conflict(b, a_text=a)


if __name__ == "__main__":
    unittest.main()
