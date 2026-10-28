"""Independent canonical preimages and paired semantic/provenance mutations."""

import copy
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.canonical import identify_profile
from vehicle_profiles.validation import HandlerPolicy, ValidationPolicies


def policies():
    return ValidationPolicies(
        profile_revisions={"synthetic.river": ("river.v1", "synthetic.v1")},
        handlers={("synthetic.ready", "v1"): HandlerPolicy(
            frozenset({"synthetic.river"}), "boolean", "", 0, 0)},
        evidence=frozenset({("synthetic/river-review", "Reference", "source_only")}),
        units=frozenset({"", "km/h"}),
        unit_conversions={("km/h", "km/h"): ("1", "0"), ("", ""): ("1", "0")},
        invalid_value_policies=frozenset({"unavailable"}), freshness_policies={})


class ProfileIdentityTests(unittest.TestCase):
    def setUp(self):
        self.profile = json.loads((ROOT / "tests/fixtures/vehicle-profiles/synthetic-river.json").read_text())
        self.policies = policies()

    def identify(self, profile=None, registry=None, revision="generator.v1", limits=None):
        return identify_profile(self.profile if profile is None else profile,
                                policies=self.policies if registry is None else registry,
                                generator_revision=revision,
                                validation_limits={} if limits is None else limits)

    def digest_pair(self, result):
        return result.profile["compatibility_identity"], result.profile["artifact_identity"]

    def test_independent_golden_preimages_and_sha256(self):
        # These tiny input records were authored separately from the expected
        # literal preimages. The fixture was never emitted by canonical.py.
        profile = {"format": "vehicle-signal-profile", "version": 1,
                   "profile_id": "golden", "semantic_policy_revision": "s1",
                   "recognition_rule_revision": "r1", "selected_bus": "can",
                   "sources": [], "messages": [], "bindings": [],
                   "diagnostics": [{"category": "all_features_disabled", "affected": "profile",
                                    "sources": [], "reason": "Synthetic empty profile"}],
                   "compatibility_identity": "0" * 64, "artifact_identity": "f" * 64}
        registry = ValidationPolicies({"golden": ("s1", "r1")}, {}, frozenset(),
                                      frozenset(), {}, frozenset(), {})
        vectors = json.loads((ROOT / "tests/fixtures/vehicle-profiles/identity-golden.json").read_text())
        for vector in vectors:
            with self.subTest(vector=vector["name"]):
                document = copy.deepcopy(profile)
                if vector["name"] == "native":
                    document["bindings"] = [{"key": "vehicle.ready", "runtime_id": 7,
                        "type": "boolean", "unit": "", "capabilities": ["read"], "choices": [],
                        "kind": "native", "inputs": [],
                        "policy": {"handler": "ready", "revision": "h1", "invalid_value": "unavailable"},
                        "evidence": {"ref": "review", "confidence": "Reference", "eligibility": "source_only"},
                        "freshness": None}]
                    document["diagnostics"] = []
                    registry = replace(registry, handlers={("ready", "h1"): HandlerPolicy(
                        frozenset({"golden"}), "boolean", "", 0, 0)}, units=frozenset({""}),
                        evidence=frozenset({("review", "Reference", "source_only")}),
                        invalid_value_policies=frozenset({"unavailable"}))
                result = self.identify(document, registry, "g1")
                self.assertEqual(result.compatibility_preimage, vector["compatibility_preimage"].encode())
                self.assertEqual(result.artifact_preimage, vector["artifact_preimage"].encode())
                self.assertEqual(self.digest_pair(result), (vector["compatibility_identity"], vector["artifact_identity"]))

    def test_digest_placeholders_are_excluded_and_input_is_unchanged(self):
        before = copy.deepcopy(self.profile)
        first = self.identify()
        self.assertEqual(self.profile, before)
        changed = copy.deepcopy(self.profile)
        changed.update(compatibility_identity="e" * 64, artifact_identity="f" * 64)
        self.assertEqual(self.digest_pair(first), self.digest_pair(self.identify(changed)))
        self.assertEqual(self.digest_pair(first), self.digest_pair(self.identify(first.profile)))

    def test_equivalent_source_and_record_order_is_byte_identical(self):
        first = self.identify()
        changed = copy.deepcopy(self.profile)
        changed["sources"].reverse()
        changed["bindings"].reverse()
        changed["messages"][0]["fields"].reverse()
        changed["messages"][0]["fields"][1]["sources"].reverse()
        changed["bindings"][1]["capabilities"].reverse()
        second = self.identify(changed)
        self.assertEqual(first.compatibility_preimage, second.compatibility_preimage)
        self.assertEqual(first.artifact_preimage, second.artifact_preimage)

    def test_exact_decimal_and_integer_encodings(self):
        first = self.identify()
        changed = copy.deepcopy(self.profile)
        field = changed["messages"][0]["fields"][0]
        field["scale"] += "0"
        field["offset"] = "-0.000e+2"
        changed["bindings"][0]["policy"]["conversion"]["scale"] = "1.000e0"
        registry = replace(self.policies, unit_conversions={("km/h", "km/h"): ("1e0", "-0"), ("", ""): ("1.0", "0.0")})
        self.assertEqual(self.digest_pair(first), self.digest_pair(self.identify(changed, registry)))
        self.assertIn(b'"scale":"0.0100000000000000000001"', first.compatibility_preimage)
        self.assertIn(b'"raw":"281474976710655"', first.artifact_preimage)
        for signed, low, high in ((False, "0", "18446744073709551615"),
                                  (True, "-9223372036854775808", "9223372036854775807")):
            wide = copy.deepcopy(self.profile)
            field = wide["messages"][0]["fields"][1]
            field.update(start=0, length=64, signed=signed, minimum=low, maximum=high,
                         value_table=[{"raw": low, "label": "Minimum"}, {"raw": high, "label": "Maximum"}])
            wide["messages"][0]["fields"] = [field]
            wide["bindings"] = wide["bindings"][1:]
            encoded = self.identify(wide).artifact_preimage
            for raw in (low, high):
                self.assertIn(('"raw":"' + raw + '"').encode(), encoded)

    def test_mechanical_changes_change_compatibility(self):
        first = self.digest_pair(self.identify())[0]
        for key, value in (("scale", "0.02"), ("offset", "1"), ("start", 1)):
            with self.subTest(key=key):
                changed = copy.deepcopy(self.profile)
                if key == "start":
                    changed["messages"][0]["fields"] = changed["messages"][0]["fields"][:1]
                    changed["diagnostics"] = []
                changed["messages"][0]["fields"][0][key] = value
                if key == "offset":
                    changed["messages"][0]["fields"][0]["minimum"] = "1"
                self.assertNotEqual(first, self.digest_pair(self.identify(changed))[0])

    def test_enum_meaning_and_raw_integer_normalization(self):
        changed = copy.deepcopy(self.profile)
        field = changed["messages"][0]["fields"][0]
        field.update(unit="", scale="1", maximum="65535", value_table=[{"raw": "-0", "label": "Stopped"}])
        binding = changed["bindings"][0]
        binding.update(type="enum", unit="", choices=[{"key": "stopped", "value": 0}, {"key": "moving", "value": 1}])
        binding["policy"]["conversion"].update(source_unit="", target_unit="", target_precision="uint16", invalid_ranges=[])
        binding["policy"]["enum_map"] = [{"raw": "-0", "choice": "stopped"}, {"raw": "1", "choice": "moving"}]
        first = self.identify(changed)
        field["value_table"][0]["raw"] = "0"
        binding["policy"]["enum_map"][0]["raw"] = "0"
        binding["choices"].reverse()
        binding["policy"]["enum_map"].reverse()
        self.assertEqual(self.digest_pair(first), self.digest_pair(self.identify(changed)))
        binding["choices"][0]["value"] = 2
        self.assertNotEqual(self.digest_pair(first)[0], self.digest_pair(self.identify(changed))[0])

    def test_native_handler_revision_and_input_order_change_compatibility(self):
        changed = copy.deepcopy(self.profile)
        changed["bindings"][1]["policy"]["revision"] = "v2"
        registry = replace(self.policies, handlers={**self.policies.handlers,
            ("synthetic.ready", "v2"): self.policies.handlers[("synthetic.ready", "v1")]})
        self.assertNotEqual(self.digest_pair(self.identify())[0], self.digest_pair(self.identify(changed, registry))[0])
        changed["bindings"][1]["inputs"] = ["river.raw_speed", "river.raw_state"]
        registry = replace(registry, handlers={("synthetic.ready", "v2"): HandlerPolicy(
            frozenset({"synthetic.river"}), "boolean", "", 2, 2)})
        first = self.identify(changed, registry)
        changed["bindings"][1]["inputs"].reverse()
        self.assertNotEqual(self.digest_pair(first)[0], self.digest_pair(self.identify(changed, registry))[0])

    def test_evidence_and_freshness_change_compatibility(self):
        first = self.digest_pair(self.identify())[0]
        changed = copy.deepcopy(self.profile)
        changed["bindings"][0]["evidence"]["eligibility"] = "operational"
        registry = replace(self.policies, evidence=self.policies.evidence | {("synthetic/river-review", "Reference", "operational")})
        self.assertNotEqual(first, self.digest_pair(self.identify(changed, registry))[0])
        changed["bindings"][0]["freshness"] = {"timeout_ms": 25, "basis": "reviewed_operational", "evidence_ref": "synthetic/river-review"}
        registry = replace(registry, freshness_policies={("synthetic.river", "vehicle.speed"): (25, "reviewed_operational", "synthetic/river-review")})
        self.assertNotEqual(first, self.digest_pair(self.identify(changed, registry))[0])

    def test_provenance_paths_license_and_source_bytes(self):
        first = self.identify()
        changed = copy.deepcopy(self.profile)
        changed["sources"][0]["path"] = "another/package.dbc"
        for key, value in (("path", "another/package.dbc"), ("license", "Reviewed synthetic attribution"), ("sha256", "f" * 64)):
            changed = copy.deepcopy(self.profile)
            changed["sources"][0][key] = value
            result = self.identify(changed)
            self.assertEqual(self.digest_pair(first)[0], self.digest_pair(result)[0])
            self.assertNotEqual(self.digest_pair(first)[1], self.digest_pair(result)[1])

    def test_runtime_assignment_is_artifact_only(self):
        first = self.identify()
        changed = copy.deepcopy(self.profile)
        changed["bindings"][0]["runtime_id"] = 42
        second = self.identify(changed)
        self.assertEqual(self.digest_pair(first)[0], self.digest_pair(second)[0])
        self.assertNotEqual(self.digest_pair(first)[1], self.digest_pair(second)[1])

    def test_generator_recognition_and_validation_inputs_affect_artifact_only(self):
        first = self.identify()
        variants = [self.identify(revision="generator.v2")]
        changed = copy.deepcopy(self.profile)
        changed["recognition_rule_revision"] = "synthetic.v2"
        variants.append(self.identify(changed, replace(self.policies,
            profile_revisions={"synthetic.river": ("river.v1", "synthetic.v2")})))
        variants.append(self.identify(registry=replace(self.policies, units=self.policies.units | {"rpm"})))
        variants.append(self.identify(limits={"synthetic_limit": 7}))
        for result in variants:
            self.assertEqual(self.digest_pair(first)[0], self.digest_pair(result)[0])
            self.assertNotEqual(self.digest_pair(first)[1], self.digest_pair(result)[1])

    def test_validation_limits_are_strict_and_order_independent(self):
        first = self.identify(limits={"a": 0, "b": 7})
        self.assertEqual(self.digest_pair(first), self.digest_pair(self.identify(limits={"b": 7, "a": 0})))
        for limits in ({"a": -1}, {"a": True}, {"a": 1.5}, {"/tmp/path": 1}, {1: 2}, []):
            with self.subTest(limits=limits), self.assertRaises(ValueError):
                self.identify(limits=limits)

    def test_generator_revision_is_explicit(self):
        for revision in ("", "/tmp/build", None):
            with self.assertRaises(ValueError):
                self.identify(revision=revision)


if __name__ == "__main__":
    unittest.main()
