"""Independent mechanics and reviewed-policy vectors, with synthetic inputs."""

import copy
import json
from pathlib import Path
import sys
import unittest
from dataclasses import replace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.bit_coordinates import occupied_bits
from vehicle_profiles.validation import (HandlerPolicy, ProfileValidationError,
                                         ValidationPolicies, validate_profile)


POLICIES = ValidationPolicies(
    profile_revisions={"synthetic.river": ("river.v1", "synthetic.v1"),
                       "synthetic.desert": ("desert.v3", "desert-recognition.v2")},
    handlers={("synthetic.ready", "v1"): HandlerPolicy(
        frozenset({"synthetic.river"}), "boolean", "", 0, 0)},
    evidence=frozenset({("synthetic/river-review", "Reference", "source_only"),
                        ("synthetic/timing", "Validated", "operational")}),
    units=frozenset({"", "km/h", "m/s"}),
    unit_conversions={("km/h", "km/h"): ("1", "0"), ("", ""): ("1", "0"),
                      ("km/h", "m/s"): ("0.27777777777777777778", "0")},
    invalid_value_policies=frozenset({"unavailable"}),
    freshness_policies={("synthetic.river", "vehicle.speed"): (10, "reviewed_operational", "synthetic/timing")},
)


class ProfileMechanicalValidationTests(unittest.TestCase):
    def setUp(self):
        self.profile = json.loads((ROOT / "tests/fixtures/vehicle-profiles/synthetic-river.json").read_text())

    def validate(self, profile=None, policies=POLICIES):
        return validate_profile(self.profile if profile is None else profile, policies=policies)

    def rejects(self, category, edit):
        document = copy.deepcopy(self.profile)
        edit(document)
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(document)
        self.assertEqual(raised.exception.category, category, str(raised.exception))
        return raised.exception

    def field_only(self, *, length=64, signed=False, start=0, order="intel"):
        p = copy.deepcopy(self.profile)
        field = p["messages"][0]["fields"][0]
        field.update(start=start, length=length, byte_order=order, signed=signed, scale="1", offset="0",
                     minimum=str(-(1 << (length - 1)) if signed else 0),
                     maximum=str((1 << (length - (1 if signed else 0))) - 1), unit="", value_table=[])
        p["messages"][0]["fields"] = [field]
        p["bindings"] = []
        p["diagnostics"] = [{"category": "all_features_disabled", "affected": "profile", "sources": [], "reason": "Synthetic mechanics only"}]
        return p

    def test_populated_profile_preserves_precision_and_unset_freshness(self):
        before = copy.deepcopy(self.profile)
        result = self.validate()
        self.assertEqual(self.profile, before)
        self.assertEqual(result["messages"][0]["fields"][0]["scale"], "0.0100000000000000000001")
        self.assertIsNone(result["bindings"][0]["freshness"])

    def test_independent_bit_walk_vectors(self):
        self.assertEqual(occupied_bits(6, 5, "intel"), (6, 7, 8, 9, 10))
        self.assertEqual(occupied_bits(2, 8, "motorola"), (2, 1, 0, 15, 14, 13, 12, 11))
        self.assertEqual(occupied_bits(7, 64, "motorola"), tuple(b for byte in range(8) for b in range(byte * 8 + 7, byte * 8 - 1, -1)))
        # A mistaken monotonically descending walk would escape at -1 here.
        self.assertNotEqual(occupied_bits(2, 8, "motorola"), tuple(range(2, -6, -1)))

    def test_cross_byte_motorola_payload_checks(self):
        self.validate(self.field_only(length=12, start=3, order="motorola"))
        p = self.field_only(length=2, start=56, order="motorola")
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p)
        self.assertEqual(raised.exception.category, "invalid_bit_coordinates")

    def test_signed_unsigned_full_width_exact_ranges(self):
        for signed, maximum in ((False, "18446744073709551615"), (True, "9223372036854775807")):
            p = self.field_only(signed=signed)
            field = p["messages"][0]["fields"][0]
            field["value_table"] = [{"raw": maximum, "label": "Synthetic maximum"}]
            self.validate(p)
            field["value_table"][0]["raw"] = str(int(maximum) + 1)
            with self.assertRaises(ProfileValidationError) as raised:
                self.validate(p)
            self.assertEqual(raised.exception.category, "invalid_enum")

    def test_invalid_decimal_mechanics(self):
        for key, value in (("scale", "0"), ("minimum", "700"), ("maximum", "700"), ("scale", "1e99999")):
            self.rejects("invalid_numeric_range", lambda p, k=key, v=value: p["messages"][0]["fields"][0].update({k: v}))

    def test_selected_bus_classic_standard_only(self):
        self.rejects("unsupported_selected_traffic", lambda p: p["messages"][0].update(identifier_format="extended"))
        self.rejects("invalid_message", lambda p: p["messages"][0].update(identifier=2048))
        self.rejects("invalid_message", lambda p: p["messages"][0].update(dlc=9))

    def test_duplicate_sources_deduplicate_without_order_winner(self):
        p = self.field_only(length=8)
        original = p["messages"][0]
        original["fields"][0]["sources"] = ["river-a"]
        duplicate = copy.deepcopy(original)
        duplicate["id"] = "a.message"
        duplicate["fields"][0].update(id="a.field", name="Different source spelling", sources=["river-b"], scale="1.00")
        p["messages"].append(duplicate)
        result = self.validate(p)
        p["messages"].reverse()
        self.assertEqual(result, self.validate(p))
        self.assertEqual(len(result["messages"]), 1)
        self.assertEqual(result["messages"][0]["fields"][0]["sources"], ["river-a", "river-b"])

    def test_conflicts_report_both_sources(self):
        for change in ("dlc", "scale", "overlap"):
            p = self.field_only(length=8)
            p["messages"][0]["fields"][0]["sources"] = ["river-a"]
            duplicate = copy.deepcopy(p["messages"][0])
            duplicate["id"] = "other.message"
            duplicate["fields"][0].update(id="other.field", sources=["river-b"])
            if change == "dlc":
                duplicate["dlc"] = 7
            elif change == "scale":
                duplicate["fields"][0].update(scale="2", maximum="510")
            else:
                duplicate["fields"][0]["start"] = 4
            p["messages"].append(duplicate)
            with self.assertRaises(ProfileValidationError) as raised:
                self.validate(p)
            self.assertEqual(raised.exception.category, "source_conflict")
            self.assertEqual(raised.exception.sources, ("river-a", "river-b"))

    def test_fragments_compose(self):
        p = self.field_only(length=8)
        fragment = copy.deepcopy(p["messages"][0])
        fragment["id"] = "fragment"
        fragment["fields"][0].update(id="fragment.field", start=8, sources=["river-b"])
        p["messages"].append(fragment)
        self.assertEqual(len(self.validate(p)["messages"][0]["fields"]), 2)

    def test_native_inputs_preserve_order_and_reject_duplicate_aliases(self):
        policies = replace(POLICIES, handlers={("synthetic.ready", "v1"): HandlerPolicy(
            frozenset({"synthetic.river"}), "boolean", "", 2, 2)})
        p = copy.deepcopy(self.profile)
        p["bindings"][1]["inputs"] = ["river.raw_state", "river.raw_speed"]
        result = self.validate(p, policies)
        self.assertEqual(result["bindings"][1]["inputs"], ["river.raw_state", "river.raw_speed"])
        self.assertEqual(self.validate(result, policies), result)
        duplicate = copy.deepcopy(p["messages"][0])
        duplicate["id"] = "duplicate.message"
        duplicate["fields"] = [duplicate["fields"][0]]
        duplicate["fields"][0]["id"] = "duplicate.speed"
        p["messages"].append(duplicate)
        p["bindings"][1]["inputs"] = ["river.raw_speed", "duplicate.speed"]
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p, policies)
        self.assertEqual(raised.exception.category, "invalid_binding")

    def test_explicit_policy_registry_and_profile_isolation(self):
        self.rejects("unknown_policy", lambda p: p.update(semantic_policy_revision="mazda.default"))
        self.rejects("unknown_policy", lambda p: p["bindings"][1]["policy"].update(handler="mazda.brake"))
        self.rejects("unknown_policy", lambda p: p.update(profile_id="another.vehicle"))
        self.rejects("unknown_policy", lambda p: p["bindings"][0]["evidence"].update(eligibility="operational"))
        self.rejects("unknown_policy", lambda p: p["bindings"][0]["policy"].update(invalid_value="hold_last"))

    def test_freshness_needs_independent_operational_evidence(self):
        self.rejects("invalid_freshness", lambda p: p["bindings"][0].update(freshness={"timeout_ms": 10, "basis": "source_timing", "evidence_ref": "synthetic/river-review"}))
        p = copy.deepcopy(self.profile)
        p["bindings"][0]["freshness"] = {"timeout_ms": 10, "basis": "reviewed_operational", "evidence_ref": "synthetic/timing"}
        self.validate(p)
        p["bindings"][0]["freshness"]["timeout_ms"] = 0
        with self.assertRaises(ProfileValidationError):
            self.validate(p)

    def test_binding_contract_and_conversion(self):
        self.rejects("invalid_capability", lambda p: p["bindings"][0].update(capabilities=["notify"]))
        self.rejects("invalid_conversion", lambda p: p["bindings"][0]["policy"]["conversion"].update(scale="2"))
        self.rejects("invalid_conversion", lambda p: p["bindings"][0]["policy"]["conversion"].update(target_precision="float64"))
        self.rejects("invalid_conversion", lambda p: p["bindings"][0]["policy"]["conversion"].update(invalid_ranges=[{"minimum": "2", "maximum": "1"}]))
        self.rejects("invalid_binding", lambda p: p["bindings"][0].update(runtime_id=65536))

    def test_float32_overflow_and_underflow_are_rejected(self):
        for scale, maximum in (("1e38", "6e42"), ("1e-50", "6e-46")):
            p = copy.deepcopy(self.profile)
            p["messages"][0]["fields"][0].update(scale=scale, maximum=maximum)
            p["bindings"][0]["policy"]["conversion"]["invalid_ranges"] = []
            with self.assertRaises(ProfileValidationError) as raised:
                self.validate(p)
            self.assertEqual(raised.exception.category, "invalid_conversion")

    def test_enum_mappings_and_boolean_contract(self):
        p = self.field_only(length=2)
        field = p["messages"][0]["fields"][0]
        p["bindings"] = [copy.deepcopy(self.profile["bindings"][0])]
        binding = p["bindings"][0]
        binding.update(type="enum", unit="", choices=[{"key": "off", "value": 0}, {"key": "on", "value": 65535}])
        binding["policy"]["conversion"].update(source_unit="", target_unit="", target_precision="uint16", invalid_ranges=[])
        binding["policy"]["enum_map"] = [{"raw": "0", "choice": "off"}, {"raw": "1", "choice": "on"}]
        self.validate(p)
        binding["policy"]["enum_map"].append({"raw": "4", "choice": "off"})
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p)
        self.assertEqual(raised.exception.category, "invalid_enum")
        binding["policy"]["enum_map"].pop()
        binding["choices"][1]["value"] = 65536
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p)
        self.assertEqual(raised.exception.category, "invalid_enum")
        binding.update(type="boolean", choices=[])
        binding["policy"]["enum_map"] = []
        binding["policy"]["conversion"]["target_precision"] = "boolean"
        field.update(length=1, maximum="1")
        self.validate(p)
        field.update(length=2, maximum="3")
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p)
        self.assertEqual(raised.exception.category, "invalid_conversion")

    def test_other_bus_field_cannot_bind_and_source_bus_must_match(self):
        self.rejects("invalid_source_reference", lambda p: p["sources"][0].update(bus="other"))
        p = copy.deepcopy(self.profile)
        p["selected_bus"] = "other"
        with self.assertRaises(ProfileValidationError) as raised:
            self.validate(p)
        self.assertEqual(raised.exception.category, "invalid_binding")

    def test_ambiguity_disables_binding_and_unmapped_remains_explicit(self):
        self.rejects("invalid_binding", lambda p: p["diagnostics"].append({"category": "ambiguous", "affected": "vehicle.speed", "sources": ["river-a", "river-b"], "reason": "Two reviewed meanings compete"}))
        p = copy.deepcopy(self.profile)
        p["bindings"].pop(0)
        p["diagnostics"].append({"category": "ambiguous", "affected": "vehicle.speed", "sources": ["river-a", "river-b"], "reason": "Two reviewed meanings compete"})
        result = self.validate(p)
        self.assertEqual({d["category"] for d in result["diagnostics"]}, {"unmapped", "ambiguous"})

    def test_empty_profile_has_no_fallback(self):
        p = json.loads((ROOT / "tests/fixtures/vehicle-profiles/synthetic-empty.json").read_text())
        result = self.validate(p)
        self.assertEqual(result["bindings"], [])
        self.assertEqual(result["messages"], [])


if __name__ == "__main__":
    unittest.main()
