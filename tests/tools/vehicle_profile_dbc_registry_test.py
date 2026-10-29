"""Derived registry, freshness and policy-isolation vectors for the DBC import (T7b)."""

import copy
from dataclasses import replace
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
DEMO_MAPPING = FIXTURES / "mapping-demo-a.json"

from generate_vehicle_profile import load_policies
from import_vehicle_profile import registry_bytes
from vehicle_profiles.dbc_import import import_profile
from vehicle_profiles.dbc_mapping import (MappingError, load_mapping, mapping_revision,
                                           semantic_revision)
from vehicle_profiles.dbc_registry import derive_registry
from vehicle_profiles.validation import ProfileValidationError, validate_profile

RPM = {"target": "vehicle.engine_rpm", "message": "Engine", "field": "rpm", "unit": "rpm"}
BRAKE = {"target": "vehicle.brake_pressure", "message": "Engine", "field": "BrakePressure",
         "unit": "bar", "conversion": {"scale": "0.01", "offset": "0"}}
LONG = dict(BRAKE, target="vehicle.acceleration.longitudinal")
GEAR = {"target": "vehicle.selector_position", "message": "Body", "field": "Gear", "unit": "",
        "choices": {"Park": "park", "Go": "drive"}}
UNITS = {"vehicle.engine_rpm": "rpm", "vehicle.brake_pressure": "kPa",
         "vehicle.acceleration.longitudinal": "kPa", "vehicle.selector_position": ""}
RPM_UNITS = {RPM["target"]: "rpm"}
REF = "mapping:synthetic.demo"


def mapping(*bindings, profile_id="synthetic.demo", **source_edit):
    document = json.loads(DEMO_MAPPING.read_text())
    document["profile_id"] = profile_id
    document["bindings"] = copy.deepcopy(list(bindings))
    document["sources"][0].update(source_edit)
    return load_mapping(json.dumps(document))


def imported_profile():
    return copy.deepcopy(import_profile(DEMO_MAPPING).profile)


def timed_import():
    """Import demo-a with a 250 ms timeout; return (profile, its derived registry)."""
    document = json.loads(DEMO_MAPPING.read_text())
    document["bindings"][0]["timeout_ms"] = 250
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copy(FIXTURES / "demo-a.dbc", tmp)
        path = Path(tmp) / "mapping.json"
        path.write_text(json.dumps(document))
        profile = import_profile(path).profile
    return copy.deepcopy(profile), derive_registry(load_mapping(json.dumps(document)), RPM_UNITS)


class RegistryVectors(unittest.TestCase):
    def test_full_registry_golden(self):
        loaded = mapping(dict(RPM, timeout_ms=250), BRAKE, GEAR)
        registry = derive_registry(loaded, UNITS)
        self.assertEqual(registry.profile_revisions, {
            "synthetic.demo": (semantic_revision(loaded), mapping_revision(loaded))})
        self.assertEqual(registry.handlers, {})
        self.assertEqual(registry.evidence, frozenset({
            (REF, "user_mapped", "source_only"), (REF, "user_mapped", "operational")}))
        self.assertEqual(registry.units, frozenset({"rpm", "bar", ""}))
        self.assertEqual(registry.unit_conversions, {
            ("rpm", "rpm"): ("1", "0"), ("kPa", "bar"): ("0.01", "0"), ("", ""): ("1", "0")})
        self.assertEqual(registry.invalid_value_policies, frozenset({"unavailable"}))
        self.assertEqual(registry.freshness_policies, {
            ("synthetic.demo", "vehicle.engine_rpm"): (250, "reviewed_operational", REF)})

    def test_registry_bytes_are_order_independent_and_reloadable(self):
        forward = derive_registry(mapping(RPM, BRAKE, GEAR), UNITS)
        reverse = derive_registry(mapping(GEAR, BRAKE, RPM), UNITS)
        self.assertEqual(registry_bytes(forward), registry_bytes(reverse))
        timed = derive_registry(mapping(dict(RPM, timeout_ms=7), BRAKE, GEAR), UNITS)
        for registry in (forward, timed):
            reloaded = load_policies(registry_bytes(registry))
            for name in ("profile_revisions", "handlers", "evidence", "units",
                         "unit_conversions", "invalid_value_policies", "freshness_policies"):
                self.assertEqual(getattr(reloaded, name), getattr(registry, name), name)

    def test_equal_decimals_share_one_row_whatever_the_order(self):
        loose = dict(LONG, conversion={"scale": "1e-2", "offset": "0.0"})
        other = dict(LONG, conversion={"scale": "0.010", "offset": "0"})
        a = derive_registry(mapping(BRAKE, loose), UNITS)
        b = derive_registry(mapping(other, BRAKE), UNITS)
        self.assertEqual(a.unit_conversions, {("kPa", "bar"): ("0.01", "0")})
        self.assertEqual(registry_bytes(a), registry_bytes(b))

    def test_reloaded_registry_validates_the_imported_profile(self):
        registry = load_policies(registry_bytes(derive_registry(mapping(RPM), RPM_UNITS)))
        validate_profile(imported_profile(), policies=registry)


class Freshness(unittest.TestCase):
    def test_no_timeout_means_no_row_and_no_operational_evidence(self):
        registry = derive_registry(mapping(RPM, BRAKE), UNITS)
        self.assertEqual(registry.freshness_policies, {})
        self.assertEqual({row[2] for row in registry.evidence}, {"source_only"})

    def test_invalid_timeouts_fail_invalid_freshness(self):
        for value in (0, -1, 4294967296, True, 1.5, "10", None):
            with self.assertRaises(MappingError) as caught:
                mapping(dict(RPM, timeout_ms=value))
            self.assertEqual(caught.exception.code, "invalid_freshness", repr(value))

    def test_bounds_become_registry_rows(self):
        for value in (1, 4294967295):
            registry = derive_registry(mapping(dict(RPM, timeout_ms=value)), RPM_UNITS)
            self.assertEqual(
                registry.freshness_policies[("synthetic.demo", RPM["target"])][0], value)

    def test_validator_needs_the_derived_row_for_declared_freshness(self):
        profile, declared = timed_import()
        validate_profile(profile, policies=declared)
        with self.assertRaises(ProfileValidationError) as caught:
            validate_profile(profile, policies=replace(declared, freshness_policies={}))
        self.assertEqual(caught.exception.category, "invalid_freshness")


class Isolation(unittest.TestCase):
    def test_each_profile_gets_only_its_own_registry(self):
        one = derive_registry(mapping(dict(RPM, timeout_ms=5), profile_id="synthetic.one"),
                              RPM_UNITS)
        two = derive_registry(mapping(BRAKE, profile_id="synthetic.two"),
                              {BRAKE["target"]: "kPa"})
        self.assertEqual(set(one.profile_revisions), {"synthetic.one"})
        self.assertEqual(set(two.profile_revisions), {"synthetic.two"})
        self.assertEqual({row[0] for row in one.evidence}, {"mapping:synthetic.one"})
        self.assertEqual({row[0] for row in two.evidence}, {"mapping:synthetic.two"})
        self.assertEqual({key[0] for key in one.freshness_policies}, {"synthetic.one"})
        self.assertEqual(two.freshness_policies, {})
        self.assertNotIn(("rpm", "rpm"), two.unit_conversions)
        self.assertNotIn("rpm", two.units)

    def test_no_native_handlers_or_unverified_opt_in_in_any_registry(self):
        registry = derive_registry(mapping(dict(RPM, timeout_ms=5), BRAKE, GEAR), UNITS)
        self.assertEqual(registry.handlers, {})
        text = registry_bytes(registry).decode().lower()
        self.assertNotIn("fresh_or_unverified", text)
        self.assertNotIn("mazda", text)
        self.assertEqual({row[1] for row in registry.freshness_policies.values()},
                         {"reviewed_operational"})
        self.assertEqual({row[1] for row in registry.evidence}, {"user_mapped"})

    def test_validator_rejects_an_unverified_freshness_basis(self):
        profile, registry = timed_import()
        profile["bindings"][0]["freshness"]["basis"] = "fresh_or_unverified"
        with self.assertRaises(ProfileValidationError) as caught:
            validate_profile(profile, policies=registry)
        self.assertEqual(caught.exception.category, "invalid_freshness")

    def test_undeclared_timeout_stays_freshness_unverified(self):
        for binding in imported_profile()["bindings"]:
            self.assertIsNone(binding["freshness"])

    def test_report_states_freshness_is_user_declared(self):
        self.assertEqual(import_profile(DEMO_MAPPING).report["freshness"], "user_declared")


class IdentityVectors(unittest.TestCase):
    def pair(self, *bindings, **source_edit):
        loaded = mapping(*bindings, **source_edit)
        return semantic_revision(loaded), mapping_revision(loaded)

    def test_numeric_unit_and_freshness_edits_change_compatibility(self):
        base = self.pair(RPM, BRAKE)
        scaled = dict(BRAKE, conversion={"scale": "0.02", "offset": "0"})
        offset = dict(BRAKE, conversion={"scale": "0.01", "offset": "1"})
        seen = {base[0]}
        for changed in (self.pair(RPM, scaled), self.pair(RPM, offset),
                        self.pair(dict(RPM, timeout_ms=9), BRAKE),
                        self.pair(dict(RPM, timeout_ms=10), BRAKE)):
            self.assertNotIn(changed[0], seen)
            seen.add(changed[0])
            self.assertNotEqual(changed[1], base[1])

    def test_provenance_edits_change_artifact_identity_only(self):
        base = self.pair(RPM)
        for edit in ({"license": "Apache-2.0; relabelled"}, {"path": "renamed.dbc"},
                     {"sha256": "f" * 64}):
            changed = self.pair(RPM, **edit)
            self.assertEqual(changed[0], base[0], edit)
            self.assertNotEqual(changed[1], base[1], edit)


if __name__ == "__main__":
    unittest.main()
