"""Context-scoped recognition and the dbc-recognition-input-v1 revision (software only)."""

import dataclasses
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from generate_vehicle_profile import load_policies
from vehicle_profiles.dbc_import import import_profile, load_request
from vehicle_profiles.recognition import (check_context, load_pack, recognition_input,
                                          recognition_rule_revision)

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
PACK_TEXT = (FIXTURES / "synthetic-pack.json").read_text()
POLICIES = load_policies((FIXTURES / "policies.json").read_text())
BASE = {"make": "synthetic", "model": "demo", "platform": None, "model_year": 2024,
        "market": "xx", "trim": None}
RESTRICTION = {"make": ["synthetic"], "model": ["demo", "demo2"], "model_year": [2024, 2025],
               "market": ["xx"]}


def restricted_pack(restriction=RESTRICTION):
    document = json.loads(PACK_TEXT)
    document["rules"][0]["context"] = restriction
    return json.dumps(document)


def run(context=None, pack_text=PACK_TEXT, pack_revision="recognition.v1"):
    """Import with policies registered for exactly this context and pack revision."""
    request = load_request((FIXTURES / "request-demo-a.json").read_text())
    request["context"] = dict(BASE, **(context or {}))
    request["pack"]["revision"] = pack_revision
    revision = recognition_rule_revision(recognition_input(
        "synthetic.demo-pack", pack_revision, request["context"]))
    policies = dataclasses.replace(POLICIES, profile_revisions={
        "synthetic.demo": ("demo.v1", revision)})
    return import_profile(request, pack=load_pack(pack_text), policies=policies,
                          base_dir=FIXTURES)


class GoldenVectors(unittest.TestCase):
    PREIMAGE = ('{"context":{"make":"synthetic","market":"xx","model":"demo","model_year":2024,'
                '"platform":null,"trim":null},"format":"dbc-recognition-input-v1",'
                '"pack_id":"synthetic.demo-pack","pack_revision":"recognition.v1"}')
    DIGEST = "65e78fbf623839d68719d8d1d6f3cbade14bf1ed79baf766c67076ccab53f614"

    def test_preimage_is_sorted_compact_and_pinned(self):
        preimage = recognition_input("synthetic.demo-pack", "recognition.v1", BASE)
        self.assertEqual(preimage, self.PREIMAGE)
        self.assertEqual(recognition_rule_revision(preimage), "recognition-v1:" + self.DIGEST)
        self.assertEqual(hashlib.sha256(self.PREIMAGE.encode()).hexdigest(), self.DIGEST)

    def test_model_year_is_an_integer_never_a_boolean_or_text(self):
        for bad in (True, False, "2024", 2024.0):
            with self.assertRaises(ValueError, msg=repr(bad)):
                check_context(dict(BASE, model_year=bad))
        self.assertIn('"model_year":null', recognition_input("p", "r", dict(BASE, model_year=None)))

    def test_text_is_ascii_codes_only(self):
        with self.assertRaises(ValueError):
            check_context(dict(BASE, trim="é"))
        self.assertNotIn("\\u", recognition_input("p", "r", BASE))

    def test_context_must_have_exactly_the_six_keys(self):
        for bad in ({k: v for k, v in BASE.items() if k != "trim"}, dict(BASE, extra=None)):
            with self.assertRaises(ValueError):
                check_context(bad)

    def test_report_retains_the_preimage_and_the_revision_enters_the_profile(self):
        result = run()
        self.assertEqual(result.report["recognition_input"],
                         {"preimage": self.PREIMAGE, "revision": "recognition-v1:" + self.DIGEST})
        self.assertEqual(result.profile["recognition_rule_revision"],
                         "recognition-v1:" + self.DIGEST)


class ContextMismatchMatrix(unittest.TestCase):
    def bound(self, context, pack_text=None):
        return len(run(context, pack_text or restricted_pack()).profile["bindings"])

    def test_matching_and_alternative_values_satisfy_the_rule(self):
        self.assertEqual(self.bound({}), 1)
        self.assertEqual(self.bound({"model": "demo2", "model_year": 2025}), 1)

    def test_wrong_values_cannot_satisfy_a_restricted_rule(self):
        for key, value in (("make", "other"), ("model", "other"), ("model_year", 2023),
                           ("market", "yy")):
            self.assertEqual(self.bound({key: value}), 0, key)

    def test_unknown_values_never_widen_a_rule(self):
        for key in RESTRICTION:
            self.assertEqual(self.bound({key: None}), 0, key)

    def test_trim_and_platform_predicates(self):
        pack = restricted_pack({"trim": ["base"], "platform": ["p1"]})
        self.assertEqual(self.bound({"trim": "base", "platform": "p1"}, pack), 1)
        for change in ({"trim": "high", "platform": "p1"}, {"trim": "base", "platform": "p2"},
                       {"trim": None, "platform": "p1"}, {"trim": "base"}):
            self.assertEqual(self.bound(change, pack), 0, change)

    def test_unrestricted_rule_ignores_context_even_when_unknown(self):
        self.assertEqual(self.bound({k: None for k in BASE}, PACK_TEXT), 1)

    def test_mismatch_disables_the_key_without_ambiguity(self):
        result = run({"market": "yy"}, restricted_pack())
        self.assertEqual(result.profile["bindings"], [])
        self.assertNotIn("ambiguous_binding", [d["code"] for d in result.report["diagnostics"]])

    def test_pack_rule_context_is_strict(self):
        for bad in ({"colour": ["red"]}, {"make": []}, {"make": [None]}, {"model_year": [True]},
                    {"model_year": ["2024"]}, {"make": "synthetic"}):
            with self.assertRaises(ValueError, msg=str(bad)):
                load_pack(restricted_pack(bad))


class RevisionAndIdentity(unittest.TestCase):
    def test_wrong_pack_revision_is_a_configuration_error(self):
        document = json.loads(PACK_TEXT)
        document["revision"] = "recognition.v2"
        with self.assertRaises(ValueError):
            run(pack_text=json.dumps(document))

    def test_unregistered_context_fails_validation_and_keeps_the_preimage(self):
        request = load_request((FIXTURES / "request-demo-a.json").read_text())
        for change in ({"model_year": 2025}, {"trim": "base"}):
            request["context"] = dict(BASE, **change)
            result = import_profile(request, pack=load_pack(PACK_TEXT), policies=POLICIES,
                                    base_dir=FIXTURES)
            self.assertIsNone(result.profile)
            self.assertEqual(result.report["diagnostics"][0]["code"], "unknown_policy")
            self.assertIn("preimage", result.report["recognition_input"])
        self.assertIsNotNone(run().profile)

    def test_context_change_alters_artifact_identity_but_not_compatibility(self):
        first, second = run(), run({"trim": "base"})
        self.assertEqual(first.profile["bindings"], second.profile["bindings"])
        self.assertNotEqual(first.profile["artifact_identity"], second.profile["artifact_identity"])
        self.assertEqual(first.profile["compatibility_identity"],
                         second.profile["compatibility_identity"])

    def test_pack_revision_change_alters_artifact_identity_only(self):
        first = run()
        document = json.loads(PACK_TEXT)
        document["revision"] = "recognition.v2"
        second = run(pack_text=json.dumps(document), pack_revision="recognition.v2")
        self.assertNotEqual(first.profile["artifact_identity"], second.profile["artifact_identity"])
        self.assertEqual(first.profile["compatibility_identity"],
                         second.profile["compatibility_identity"])

    def test_changed_bindings_change_compatibility_identity(self):
        other = run({"market": "yy"}, restricted_pack())
        self.assertNotEqual(run().profile["compatibility_identity"],
                            other.profile["compatibility_identity"])


if __name__ == "__main__":
    unittest.main()
