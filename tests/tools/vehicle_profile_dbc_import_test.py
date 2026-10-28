"""Walking-skeleton import: demo-a through mapping.json to a validated v1 profile."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.dbc_import import import_profile
from vehicle_profiles.dbc_mapping import (MappingError, load_mapping, mapping_revision,
                                           semantic_revision)
from vehicle_profiles.dbc_registry import derive_registry
from vehicle_profiles.validation import validate_profile

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
MAPPING = FIXTURES / "mapping-demo-a.json"
DEMO_A = (FIXTURES / "demo-a.dbc").read_text()


def snapshot(directory):
    return sorted((str(p), p.stat().st_size, p.stat().st_mtime_ns)
                  for p in directory.rglob("*") if p.is_file())


def staged(mutate=None, dbc=None, mapping="mapping-demo-a.json", keep_digest=False):
    """Copy a fixture mapping and demo-a into a temp dir; return the mapping path."""
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    text = DEMO_A if dbc is None else dbc
    (root / "demo-a.dbc").write_text(text)
    document = json.loads((FIXTURES / mapping).read_text())
    if dbc is not None and not keep_digest:
        import hashlib
        document["sources"][0]["sha256"] = hashlib.sha256(text.encode()).hexdigest()
    if mutate is not None:
        mutate(document)
    path = root / "mapping.json"
    path.write_text(json.dumps(document))
    staged.keep.append(tmp)
    return path


staged.keep = []


def codes(result):
    return [item["code"] for item in result.report["diagnostics"]]


class AcceptanceTests(unittest.TestCase):
    def test_demo_a_binds_engine_rpm_and_leaves_pressure_unmapped(self):
        result = import_profile(MAPPING)
        profile = result.profile
        (binding,) = profile["bindings"]
        self.assertEqual(binding["key"], "vehicle.engine_rpm")
        (message,) = profile["messages"]
        by_name = {field["name"]: field for field in message["fields"]}
        self.assertEqual(binding["inputs"], [by_name["rpm"]["id"]])
        self.assertTrue(by_name["rpm"]["id"].startswith("dbc-field-v1:"))
        unmapped = [d for d in profile["diagnostics"] if d["category"] == "unmapped"]
        self.assertEqual([d["affected"] for d in unmapped], [by_name["BrakePressure"]["id"]])
        self.assertEqual(codes(result), ["unmapped_field"])
        self.assertEqual(result.report["diagnostics"][0]["severity"], "info")
        self.assertEqual(result.report["diagnostics"][0]["source_ids"], ["demo-a"])

    def test_empty_bindings_yield_explicit_all_features_disabled_without_fallback(self):
        result = import_profile(FIXTURES / "mapping-empty.json")
        self.assertEqual(result.profile["bindings"], [])
        categories = [d["category"] for d in result.profile["diagnostics"]]
        self.assertEqual(categories.count("all_features_disabled"), 1)
        self.assertEqual(categories.count("unmapped"), 2)
        self.assertNotIn("vehicle.", json.dumps(result.profile))
        self.assertEqual(codes(result)[-1], "all_features_disabled")

    def test_output_passes_unmodified_validator_and_has_identities(self):
        result = import_profile(MAPPING)
        validate_profile(result.profile, policies=derive_registry(load_mapping(MAPPING.read_text())))
        self.assertRegex(result.profile["compatibility_identity"], r"^[0-9a-f]{64}$")
        self.assertRegex(result.profile["artifact_identity"], r"^[0-9a-f]{64}$")
        self.assertEqual(result.identity.profile, result.profile)
        mapping = load_mapping(MAPPING.read_text())
        self.assertEqual(result.profile["semantic_policy_revision"], semantic_revision(mapping))
        self.assertEqual(result.profile["recognition_rule_revision"], mapping_revision(mapping))
        self.assertNotEqual(semantic_revision(mapping), mapping_revision(mapping))

    def test_v1_diagnostic_shape_is_not_extended(self):
        for diagnostic in import_profile(MAPPING).profile["diagnostics"]:
            self.assertEqual(set(diagnostic), {"category", "affected", "sources", "reason"})

    def test_run_is_deterministic_and_leaves_no_path_or_tree_change(self):
        before = snapshot(FIXTURES)
        first, second = import_profile(MAPPING), import_profile(MAPPING)
        self.assertEqual(snapshot(FIXTURES), before)
        self.assertEqual(first.identity.artifact_preimage, second.identity.artifact_preimage)
        text = json.dumps([first.profile, first.report])
        self.assertNotIn(str(FIXTURES), text)
        self.assertEqual(first.profile["sources"][0]["path"], "demo-a.dbc")

    def test_no_registry_request_or_pack_file_is_committed(self):
        names = sorted(p.name for p in FIXTURES.iterdir())
        self.assertEqual(names, ["PROVENANCE.md", "demo-a.dbc", "demo-b.dbc",
                                 "mapping-demo-a.json", "mapping-empty.json"])


class DerivedRegistryTests(unittest.TestCase):
    def test_registry_contains_only_the_derived_entries(self):
        mapping = load_mapping(MAPPING.read_text())
        registry = derive_registry(mapping)
        self.assertRegex(mapping_revision(mapping), r"^mapping-v1:[0-9a-f]{64}$")
        self.assertEqual(registry.profile_revisions, {"synthetic.demo": (
            semantic_revision(mapping), mapping_revision(mapping))})
        self.assertEqual(registry.handlers, {})
        self.assertEqual(registry.evidence,
                         frozenset({("mapping:synthetic.demo", "user_mapped", "source_only")}))
        self.assertEqual(registry.units, frozenset({"rpm"}))
        self.assertEqual(registry.unit_conversions, {("rpm", "rpm"): ("1", "0")})
        self.assertEqual(registry.invalid_value_policies, frozenset({"unavailable"}))
        self.assertEqual(registry.freshness_policies, {})

    def test_semantic_revision_excludes_sources_and_ignores_order(self):
        base = load_mapping(MAPPING.read_text())
        other = json.loads(MAPPING.read_text())
        other["sources"][0]["license"] = "another note"
        other["sources"][0]["path"] = "sub/demo-a.dbc"
        other = load_mapping(json.dumps(other))
        self.assertEqual(semantic_revision(other), semantic_revision(base))
        self.assertNotEqual(mapping_revision(other), mapping_revision(base))
        reordered = json.loads(MAPPING.read_text())
        reordered["units"] = ["rpm"]
        reordered["bindings"].reverse()
        reordered = load_mapping(json.dumps(reordered, sort_keys=True))
        self.assertEqual(mapping_revision(reordered), mapping_revision(base))

    def test_license_or_path_edit_changes_artifact_but_not_compatibility_identity(self):
        base = import_profile(MAPPING)
        for change in (lambda d: d["sources"][0].update(license="Apache-2.0; edited note"),
                       lambda d: d["sources"][0].update(path="demo-a.dbc")):
            edited = import_profile(staged(change))
            self.assertEqual(edited.profile["compatibility_identity"],
                             base.profile["compatibility_identity"])
        self.assertNotEqual(import_profile(staged(
            lambda d: d["sources"][0].update(license="edited"))).profile["artifact_identity"],
            base.profile["artifact_identity"])

    def test_path_edit_changes_artifact_but_not_compatibility_identity(self):
        base = import_profile(MAPPING)
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "sub").mkdir()
            Path(tmp, "sub", "demo-a.dbc").write_text(DEMO_A)
            document = json.loads(MAPPING.read_text())
            document["sources"][0]["path"] = "sub/demo-a.dbc"
            Path(tmp, "mapping.json").write_text(json.dumps(document))
            moved = import_profile(Path(tmp, "mapping.json"))
        self.assertEqual(moved.profile["compatibility_identity"],
                         base.profile["compatibility_identity"])
        self.assertNotEqual(moved.profile["artifact_identity"], base.profile["artifact_identity"])

    def test_unit_conversion_edit_changes_compatibility_identity(self):
        base = import_profile(MAPPING)
        edited = import_profile(staged(
            lambda d: d.update(unit_conversions=[["rpm", "rpm", "2", "0"]])))
        self.assertNotEqual(edited.profile["compatibility_identity"],
                            base.profile["compatibility_identity"])

    def test_imported_profile_cannot_carry_native_handler_or_brake_opt_in(self):
        text = json.dumps(import_profile(MAPPING).profile)
        for word in ("native", "fresh_or_unverified", "handler"):
            self.assertNotIn(word, text)


class LoaderTests(unittest.TestCase):
    def good(self):
        return json.loads(MAPPING.read_text())

    def rejects(self, text):
        with self.assertRaises(MappingError):
            load_mapping(text)

    def test_strict_json_rejects_duplicate_keys_and_nonfinite_numbers(self):
        self.rejects('{"version": 1, "version": 1}')
        self.rejects('{"version": NaN}')
        self.rejects('{"version": Infinity}')
        self.rejects("[]")
        self.rejects("{")

    def test_exact_shape(self):
        for mutate in (lambda d: d.update(extra=1), lambda d: d.pop("units"),
                       lambda d: d.update(version=2), lambda d: d.update(version=True),
                       lambda d: d["bindings"][0].update(extra=1),
                       lambda d: d["sources"][0].pop("sha256"),
                       lambda d: d["sources"][0].update(sha256="ABC"),
                       lambda d: d["sources"][0].update(bus="other"),
                       lambda d: d["sources"][0].update(encoding="latin-1"),
                       lambda d: d.update(sources=[]),
                       lambda d: d["unit_conversions"].append(["rpm", "rpm", "2", "0"]),
                       lambda d: d["unit_conversions"][0].__setitem__(2, "x"),
                       lambda d: d["units"].append("rpm")):
            document = self.good()
            mutate(document)
            self.rejects(json.dumps(document))

    def test_source_paths_stay_inside_the_profile_directory(self):
        for path in ("/etc/x.dbc", "../x.dbc", "a/../../x.dbc", "a\\b.dbc", "", "C:x.dbc"):
            document = self.good()
            document["sources"][0]["path"] = path
            self.rejects(json.dumps(document))

    def test_freshness_policies_are_completed_in_a_later_ticket(self):
        document = self.good()
        document["freshness_policies"] = [["vehicle.engine_rpm", 100]]
        self.rejects(json.dumps(document))

    def test_dbc_text_is_never_evaluated(self):
        dbc = DEMO_A.replace("synthetic-a-v1", "__import__('os')")
        self.assertEqual(len(import_profile(staged(dbc=dbc)).profile["bindings"]), 1)


class MappingResolutionTests(unittest.TestCase):
    def fatal(self, result, code):
        self.assertIsNone(result.profile)
        self.assertIsNone(result.identity)
        self.assertIn(code, codes(result))
        for item in result.report["diagnostics"]:
            self.assertEqual(item["severity"], "error")
        self.assertNotIn(str(FIXTURES), json.dumps(result.report))
        self.assertNotIn(tempfile.gettempdir(), json.dumps(result.report))

    def test_unknown_target(self):
        result = import_profile(staged(lambda d: d["bindings"][0].update(target="vehicle.nope")))
        self.fatal(result, "unknown_target")

    def test_duplicate_target(self):
        def mutate(d):
            d["bindings"].append(dict(d["bindings"][0], field="BrakePressure"))
        self.fatal(import_profile(staged(mutate)), "duplicate_target")

    def test_stale_message_and_field(self):
        self.fatal(import_profile(staged(lambda d: d["bindings"][0].update(message="Gone"))),
                   "stale_mapping")
        self.fatal(import_profile(staged(lambda d: d["bindings"][0].update(field="Gone"))),
                   "stale_mapping")

    def test_number_field_cannot_feed_a_boolean_target(self):
        def mutate(d):
            d["bindings"][0].update(target="vehicle.brake_pressed", field="BrakePressure")
        self.fatal(import_profile(staged(mutate)), "incompatible_target")

    def test_undeclared_unit_conversion_is_incompatible(self):
        def mutate(d):
            d["bindings"][0].update(field="BrakePressure", unit="rpm")
        self.fatal(import_profile(staged(mutate)), "incompatible_target")

    def test_digest_mismatch_is_fatal(self):
        result = import_profile(staged(lambda d: d["sources"][0].update(sha256="0" * 64)))
        self.fatal(result, "source_digest_mismatch")
        self.assertEqual(result.report["diagnostics"][0]["stage"], "acquisition")

    def test_missing_source_is_source_io_without_a_path(self):
        result = import_profile(staged(lambda d: d["sources"][0].update(path="missing.dbc")))
        self.fatal(result, "source_io")

    def test_syntax_error_is_fatal_with_location(self):
        result = import_profile(staged(dbc='VERSION "x"\nBO_ nonsense\n'))
        self.fatal(result, "dbc_syntax_error")
        self.assertEqual(result.report["diagnostics"][0]["locations"][0]["source_id"], "demo-a")

    def test_validator_category_is_forwarded(self):
        result = import_profile(staged(lambda d: d["units"].clear()))
        self.fatal(result, "unknown_policy")
        self.assertEqual(result.report["diagnostics"][0]["source_ids"], ["demo-a"])

    def test_unreadable_or_malformed_mapping_is_a_configuration_error(self):
        with self.assertRaises(MappingError):
            import_profile(FIXTURES / "does-not-exist.json")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "m.json").write_text("{")
            with self.assertRaises(MappingError):
                import_profile(Path(tmp, "m.json"))


class SourceReportTests(unittest.TestCase):
    WIDE = 'BO_ 2147483905 Wide: 8 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n'

    def test_unsupported_message_is_inventory_only(self):
        result = import_profile(staged(dbc=DEMO_A + self.WIDE))
        self.assertEqual(len(result.profile["messages"]), 1)
        self.assertIn("unsupported", [d["category"] for d in result.profile["diagnostics"]])
        self.assertEqual(codes(result), ["unsupported_message", "unmapped_field"])

    def test_mapping_an_unsupported_message_fails_required_binding(self):
        def mutate(d):
            d["bindings"].append({"target": "vehicle.speed_kph", "message": "Wide",
                                  "field": "X", "unit": "rpm"})
        result = import_profile(staged(mutate, dbc=DEMO_A + self.WIDE))
        self.assertIsNone(result.profile)
        self.assertIn("unsupported_required_binding", codes(result))

    def test_report_shape_and_sort_order(self):
        result = import_profile(staged(dbc=DEMO_A + self.WIDE, mapping="mapping-empty.json"))
        stages = ["acquisition", "parse", "mapping", "validation", "result"]
        keys = [(stages.index(i["stage"]), i["source_ids"],
                 [(l["line"], l["column"], l["end_line"], l["end_column"]) for l in i["locations"]],
                 i["code"], i["affected"]) for i in result.report["diagnostics"]]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(codes(result)[-1], "all_features_disabled")
        for item in result.report["diagnostics"]:
            self.assertEqual(set(item), {"code", "severity", "stage", "affected", "source_ids",
                                         "locations", "reason"})
        self.assertEqual(set(result.report), {"profile_id", "semantic_revision",
                                              "mapping_revision", "sources", "diagnostics"})

    def test_report_lists_source_with_digest_and_no_path(self):
        (source,) = import_profile(MAPPING).report["sources"]
        self.assertEqual(set(source), {"id", "logical_path", "bus", "sha256", "version"})
        self.assertEqual((source["id"], source["logical_path"]), ("demo-a", "demo-a.dbc"))
        self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
