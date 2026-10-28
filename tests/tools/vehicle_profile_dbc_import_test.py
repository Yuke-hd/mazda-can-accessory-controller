"""Walking-skeleton import: one synthetic source to a validated, identified v1 profile."""

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from generate_vehicle_profile import load_policies
from vehicle_profiles.dbc_import import import_profile, load_request
from vehicle_profiles.recognition import load_pack
from vehicle_profiles.validation import validate_profile

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
PACK_TEXT = (FIXTURES / "synthetic-pack.json").read_text()
POLICIES = load_policies((FIXTURES / "policies.json").read_text())


def request(**changes):
    value = load_request((FIXTURES / "request-demo-a.json").read_text())
    value.update(changes)
    return value


def run(req=None, pack_text=PACK_TEXT, base_dir=FIXTURES):
    return import_profile(request() if req is None else req, pack=load_pack(pack_text),
                          policies=POLICIES, base_dir=base_dir)


def pack_with(mutate):
    document = json.loads(PACK_TEXT)
    mutate(document)
    return json.dumps(document)


def codes(result):
    return [item["code"] for item in result.report["diagnostics"]]


class AcceptanceTests(unittest.TestCase):
    def test_demo_a_binds_engine_rpm_and_leaves_pressure_unmapped(self):
        result = run()
        profile = result.profile
        (binding,) = profile["bindings"]
        self.assertEqual(binding["key"], "vehicle.engine_rpm")
        (message,) = profile["messages"]
        by_name = {field["name"]: field for field in message["fields"]}
        self.assertEqual(binding["inputs"], [by_name["rpm"]["id"]])
        self.assertEqual((by_name["rpm"]["scale"], by_name["rpm"]["maximum"]), ("0.25", "16383.75"))
        unmapped = [d for d in profile["diagnostics"] if d["category"] == "unmapped"]
        self.assertEqual([d["affected"] for d in unmapped], [by_name["BrakePressure"]["id"]])
        self.assertEqual(unmapped[0]["sources"], ["demo-a"])
        self.assertEqual([d for d in codes(result)], ["unmapped_field"])
        self.assertEqual(result.report["diagnostics"][0]["source_ids"], ["demo-a"])

    def test_request_assigns_no_per_field_owner_mapping(self):
        for name in ("request-demo-a.json", "request-mazda-custom.json"):
            text = (FIXTURES / name).read_text()
            for word in ("binding", "field", "signal", "rpm", "BrakePressure"):
                self.assertNotIn(word.lower(), text.replace("max_fields", "").lower(), name)

    def test_no_matching_rule_yields_explicit_empty_profile_without_fallback(self):
        pack = pack_with(lambda d: d["rules"][0]["match"].update(aliases=["NoSuchSignal"]))
        result = run(pack_text=pack)
        self.assertEqual(result.profile["bindings"], [])
        categories = [d["category"] for d in result.profile["diagnostics"]]
        self.assertIn("all_features_disabled", categories)
        self.assertEqual(categories.count("unmapped"), 2)
        self.assertIn("all_features_disabled", codes(result))
        self.assertNotIn("vehicle.engine_rpm", json.dumps(result.profile))

    def test_output_passes_unmodified_validator_and_has_identities(self):
        result = run()
        validate_profile(result.profile, policies=POLICIES)
        self.assertRegex(result.profile["compatibility_identity"], r"^[0-9a-f]{64}$")
        self.assertRegex(result.profile["artifact_identity"], r"^[0-9a-f]{64}$")
        self.assertEqual(result.identity.profile, result.profile)

    def test_v1_diagnostic_shape_is_not_extended(self):
        for diagnostic in run().profile["diagnostics"]:
            self.assertEqual(set(diagnostic), {"category", "affected", "sources", "reason"})

    def test_run_is_deterministic_and_carries_no_machine_path_or_timestamp(self):
        first, second = run(), run()
        self.assertEqual(first.identity.artifact_preimage, second.identity.artifact_preimage)
        text = json.dumps([first.profile, first.report])
        self.assertNotIn(str(FIXTURES), text)
        self.assertNotIn("demo-a.dbc\"", text.replace("synthetic/demo-a.dbc", ""))
        self.assertEqual(first.profile["sources"][0]["path"], "synthetic/demo-a.dbc")


class RecognitionTests(unittest.TestCase):
    def test_aliases_are_ascii_case_insensitive(self):
        pack = pack_with(lambda d: d["rules"][0]["match"].update(aliases=["RPM"]))
        self.assertEqual(len(run(pack_text=pack).profile["bindings"]), 1)

    def test_unit_predicate_must_match_exactly(self):
        pack = pack_with(lambda d: d["rules"][0]["match"].update(unit="kPa"))
        self.assertEqual(run(pack_text=pack).profile["bindings"], [])

    def test_mechanical_predicate_must_match(self):
        for key, value in (("length", 8), ("byte_order", "motorola"), ("signed", True)):
            pack = pack_with(lambda d: d["rules"][0]["match"].update({key: value}))
            self.assertEqual(run(pack_text=pack).profile["bindings"], [], key)

    def test_spelling_similarity_or_substring_does_not_match(self):
        pack = pack_with(lambda d: d["rules"][0]["match"].update(aliases=["rp", "rpm_extra"]))
        self.assertEqual(run(pack_text=pack).profile["bindings"], [])

    def test_repeated_rule_for_the_same_field_does_not_duplicate(self):
        pack = pack_with(lambda d: d["rules"].append(copy.deepcopy(d["rules"][0])))
        self.assertEqual(len(run(pack_text=pack).profile["bindings"]), 1)

    def test_conflicting_rule_templates_fail_closed_in_either_order(self):
        def conflicting(first):
            def mutate(d):
                copy_ = copy.deepcopy(d["rules"][0])
                copy_["binding"]["runtime_id"] = 7
                d["rules"].insert(0 if first else 1, copy_)
            return mutate
        results = [run(pack_text=pack_with(conflicting(first))) for first in (True, False)]
        for result in results:
            self.assertEqual(result.profile["bindings"], [])
            self.assertIn("ambiguous_binding", codes(result))
        self.assertEqual(results[0].identity.artifact_preimage,
                         results[1].identity.artifact_preimage)

    def test_diagnostic_overflow_is_reported_after_recognition(self):
        result = run(request(limits=dict(request()["limits"], max_diagnostics=0)))
        self.assertEqual(result.report["diagnostics"][0]["stage"], "result")

    def test_distinct_fields_for_one_key_are_ambiguous(self):
        text = (FIXTURES / "demo-a.dbc").read_text().replace(
            "BrakePressure", "engine_rpm").replace('"kPa"', '"rpm"').replace(
            "0.1,0) [0|6553.5]", "0.25,0) [0|16383.75]")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "demo-a.dbc").write_text(text)
            result = run(base_dir=Path(tmp))
        self.assertEqual(result.profile["bindings"], [])
        self.assertIn("ambiguous", [d["category"] for d in result.profile["diagnostics"]])
        self.assertIn("ambiguous_binding", codes(result))
        self.assertIn("all_features_disabled", codes(result))


class SourceReportTests(unittest.TestCase):
    def test_unsupported_message_is_inventory_only(self):
        text = (FIXTURES / "demo-a.dbc").read_text() + (
            'BO_ 2147483905 Wide: 8 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n')
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "demo-a.dbc").write_text(text)
            result = run(base_dir=Path(tmp))
        self.assertEqual(len(result.profile["messages"]), 1)
        self.assertIn("unsupported", [d["category"] for d in result.profile["diagnostics"]])
        self.assertEqual(codes(result), ["unsupported_message", "unmapped_field"])

    def test_report_is_sorted_by_stage_source_location_code_and_affected(self):
        result = run(pack_text=pack_with(lambda d: d["rules"][0]["match"].update(aliases=["x"])))
        keys = [(item["stage"], item["source_ids"], item["locations"], item["code"],
                 item["affected"]) for item in result.report["diagnostics"]]
        self.assertEqual(keys, sorted(keys, key=lambda k: (
            ["parse", "recognition", "result"].index(k[0]), k[1],
            [(l["line"], l["column"]) for l in k[2]], k[3], k[4])))
        self.assertEqual(codes(result)[-1], "all_features_disabled")
        for item in result.report["diagnostics"]:
            self.assertEqual(set(item), {"code", "severity", "stage", "affected", "source_ids",
                                         "locations", "reason"})

    def test_report_lists_source_with_digest_and_no_path(self):
        (source,) = run().report["sources"]
        self.assertEqual(source["id"], "demo-a")
        self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(source["logical_path"], "synthetic/demo-a.dbc")
        self.assertNotIn("path", source)


class FatalTests(unittest.TestCase):
    def fatal(self, result, code):
        self.assertIsNone(result.profile)
        self.assertIsNone(result.identity)
        (item,) = result.report["diagnostics"]
        self.assertEqual((item["code"], item["severity"]), (code, "error"))
        self.assertNotIn(str(FIXTURES), json.dumps(result.report))

    def test_missing_file_is_source_io_without_a_path(self):
        source = dict(request()["sources"][0], path="missing.dbc")
        self.fatal(run(request(sources=[source])), "source_io")

    def test_digest_mismatch_is_fatal(self):
        source = dict(request()["sources"][0], expected_sha256="0" * 64)
        self.fatal(run(request(sources=[source])), "source_digest_mismatch")

    def test_missing_provenance_is_fatal(self):
        source = dict(request()["sources"][0], license_ref="")
        self.fatal(run(request(sources=[source])), "source_provenance_missing")

    def test_syntax_error_is_fatal_with_location(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "demo-a.dbc").write_text('VERSION "x"\nBO_ nonsense\n')
            result = run(base_dir=Path(tmp))
        self.fatal(result, "dbc_syntax_error")
        self.assertEqual(result.report["diagnostics"][0]["locations"][0]["source_id"], "demo-a")

    def test_limit_overflow_is_fatal(self):
        limits = dict(request()["limits"], max_fields=1)
        self.fatal(run(request(limits=limits)), "resource_overflow")

    def test_validator_category_is_forwarded(self):
        pack = pack_with(lambda d: d["rules"][0]["binding"]["evidence"].update(ref="unknown/ref"))
        result = run(pack_text=pack)
        self.fatal(result, "unknown_policy")
        self.assertEqual(result.report["diagnostics"][0]["source_ids"], ["demo-a"])


class MazdaBaselineTests(unittest.TestCase):
    """The committed repository DBC, referenced in place, through the synthetic pack."""

    SOURCE = ROOT / "docs/protocol/mazda_custom.dbc"

    def run_baseline(self):
        req = load_request((FIXTURES / "request-mazda-custom.json").read_text())
        return run(req, base_dir=ROOT)

    def test_baseline_imports_to_a_validated_profile_without_fatal_items(self):
        result = self.run_baseline()
        self.assertIsNotNone(result.profile)
        validate_profile(result.profile, policies=POLICIES)
        self.assertEqual(result.identity.profile, result.profile)
        self.assertNotIn("error", [item["severity"] for item in result.report["diagnostics"]])
        self.assertEqual(len(result.profile["messages"]), 9)
        fields = [f for m in result.profile["messages"] for f in m["fields"]]
        self.assertEqual(len(fields), 22)

    def test_synthetic_pack_binds_nothing_so_the_profile_is_explicitly_empty(self):
        # EngineRPM is Motorola (@0); the reviewed synthetic rule requires Intel.
        result = self.run_baseline()
        self.assertEqual(result.profile["bindings"], [])
        self.assertEqual(codes(result), ["unmapped_field"] * 22 + ["all_features_disabled"])
        by_name = {f["name"]: f for m in result.profile["messages"] for f in m["fields"]}
        self.assertEqual(by_name["EngineRPM"]["byte_order"], "motorola")
        unmapped = [d["affected"] for d in result.profile["diagnostics"]
                    if d["category"] == "unmapped"]
        self.assertIn(by_name["EngineRPM"]["id"], unmapped)
        self.assertNotIn("vehicle.", json.dumps(result.profile))

    def test_report_lists_the_mazda_source_with_its_digest(self):
        (source,) = self.run_baseline().report["sources"]
        self.assertEqual(source["id"], "mazda-custom")
        self.assertEqual(source["logical_path"], "docs/protocol/mazda_custom.dbc")
        self.assertEqual(source["bus"], "body")
        self.assertEqual(source["sha256"], hashlib.sha256(self.SOURCE.read_bytes()).hexdigest())
        self.assertNotIn("path", source)

    def test_output_carries_no_absolute_path(self):
        result = self.run_baseline()
        text = json.dumps([result.profile, result.report])
        for path in (ROOT, FIXTURES, self.SOURCE):
            self.assertNotIn(str(path), text)
        self.assertNotIn(str(Path.home()), text)

    def test_repeated_run_is_byte_identical(self):
        first, second = self.run_baseline(), self.run_baseline()
        dump = lambda r: json.dumps([r.profile, r.report], sort_keys=True).encode()
        self.assertEqual(dump(first), dump(second))
        self.assertEqual(first.identity.artifact_preimage, second.identity.artifact_preimage)
        self.assertEqual(first.identity.compatibility_preimage,
                         second.identity.compatibility_preimage)


class ConfigurationTests(unittest.TestCase):
    def test_pack_must_match_the_request_reference_and_profile(self):
        for change in (lambda d: d.update(id="other"), lambda d: d.update(revision="other"),
                       lambda d: d.update(profile_id="other.profile")):
            with self.assertRaises(ValueError):
                run(pack_text=pack_with(change))

    def test_exactly_one_selected_bus_source_is_required(self):
        source = request()["sources"][0]
        for sources in ([], [source, dict(source, id="demo-b")], [dict(source, bus="other")]):
            with self.assertRaises(ValueError):
                run(request(sources=sources))

    def test_request_and_pack_are_strict_json(self):
        text = (FIXTURES / "request-demo-a.json").read_text()
        for bad in ('{"version": 1, "version": 1}', '{"version": NaN}', "[]",
                    text.replace('"version": 1,', '"version": 1, "extra": 1,', 1)):
            with self.assertRaises(ValueError):
                load_request(bad)
        for bad in ('{"version": 1, "version": 1}', '{"version": Infinity}',
                    pack_with(lambda d: d.update(extra=1)),
                    pack_with(lambda d: d["rules"][0]["match"].update(aliases=[])),
                    pack_with(lambda d: d["rules"][0]["match"].update(aliases=["a", "A"])),
                    pack_with(lambda d: d["rules"][0]["match"].update(aliases=["é"])),
                    pack_with(lambda d: d["rules"][0]["match"].pop("unit"))):
            with self.assertRaises(ValueError):
                load_pack(bad)

    def test_dbc_text_is_never_evaluated(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "demo-a.dbc").write_text('VERSION "__import__(\'os\')"\n' + "".join(
                (FIXTURES / "demo-a.dbc").read_text().splitlines(True)[1:]))
            result = run(base_dir=Path(tmp))
        self.assertEqual(len(result.profile["bindings"]), 1)


if __name__ == "__main__":
    unittest.main()
