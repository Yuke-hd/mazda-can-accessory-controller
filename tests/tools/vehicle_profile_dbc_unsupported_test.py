"""T6: unsupported DBC wire forms are inventoried, never executable, and fail required bindings."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.dbc_import import import_profile

FIXTURES = ROOT / "tests/fixtures/vehicle-profiles/dbc"
DEMO_A = (FIXTURES / "demo-a.dbc").read_text()
FD_DEFS = ('BA_DEF_ BO_ "VFrameFormat" ENUM '
           '"StandardCAN","ExtendedCAN","StandardCAN_FD","ExtendedCAN_FD";\n')

# name -> (extra DBC text, expected unsupported form, message name, field name)
FORMS = {
    "extended_id": ('BO_ 2147483905 Odd: 8 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n',
                    "extended_id", "Odd", "X"),
    "length_over_8": ('BO_ 600 Odd: 12 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n',
                      "length_over_8", "Odd", "X"),
    "can_fd": (FD_DEFS + 'BO_ 601 Odd: 8 ECU\n SG_ X : 0|8@1+ (1,0) [0|255] "" Controller\n'
               'BA_ "VFrameFormat" BO_ 601 2;\n', "can_fd", "Odd", "X"),
    "multiplex": ('BO_ 602 Odd: 8 ECU\n SG_ sel M : 0|8@1+ (1,0) [0|0] "" Controller\n'
                  ' SG_ X m0 : 8|8@1+ (1,0) [0|0] "" Controller\n'
                  ' SG_ plain : 16|8@1+ (1,0) [0|0] "" Controller\n', "multiplex", "Odd", "plain"),
    "ieee_float": ('BO_ 603 Odd: 8 ECU\n SG_ X : 0|32@1- (1,0) [0|0] "" Controller\n'
                   ' SG_ Y : 32|8@1+ (1,0) [0|0] "" Controller\n'
                   'SIG_VALTYPE_ 603 X : 1;\n', "ieee_float", "Odd", "Y"),
    "ieee_double": ('BO_ 604 Odd: 8 ECU\n SG_ X : 0|64@1- (1,0) [0|0] "" Controller\n'
                    'SIG_VALTYPE_ 604 X : 2;\n', "ieee_double", "Odd", "X"),
}


def run(extra, binding=None, mapping="mapping-demo-a.json"):
    text = DEMO_A + extra
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "demo-a.dbc").write_text(text)
        document = json.loads((FIXTURES / mapping).read_text())
        document["sources"][0]["sha256"] = hashlib.sha256(text.encode()).hexdigest()
        if binding:
            document["bindings"].append(
                {"target": "vehicle.speed_kph", "message": binding[0], "field": binding[1],
                 "unit": "rpm"})
        (root / "mapping.json").write_text(json.dumps(document))
        return import_profile(root / "mapping.json")


def codes(result):
    return [i["code"] for i in result.report["diagnostics"]]


class UnsupportedFormTests(unittest.TestCase):
    def test_each_form_is_inventory_only_without_a_binding(self):
        for name, (extra, form, _message, _field) in FORMS.items():
            with self.subTest(name):
                result = run(extra)
                self.assertEqual([m["identifier"] for m in result.profile["messages"]], [256])
                (item,) = [i for i in result.report["diagnostics"]
                           if i["code"] == "unsupported_message"]
                self.assertEqual(item["severity"], "warning")
                self.assertIn(form, item["reason"])
                self.assertGreater(item["locations"][0]["line"], 0)
                self.assertNotIn("unsupported_required_binding", codes(result))
                self.assertEqual([b["key"] for b in result.profile["bindings"]],
                                 ["vehicle.engine_rpm"])

    def test_each_form_fails_a_binding_that_requires_it(self):
        for name, (extra, _form, message, field) in FORMS.items():
            with self.subTest(name):
                result = run(extra, binding=(message, field))
                self.assertIsNone(result.profile)
                (item,) = [i for i in result.report["diagnostics"]
                           if i["code"] == "unsupported_required_binding"]
                self.assertEqual(item["severity"], "error")
                self.assertTrue(item["locations"])

    def test_unsupported_message_with_no_bindings_is_all_features_disabled(self):
        result = run(FORMS["multiplex"][0], mapping="mapping-empty.json")
        self.assertIn("unsupported_message", codes(result))
        self.assertEqual(codes(result)[-1], "all_features_disabled")
        self.assertEqual(len(result.profile["messages"]), 1)

    def test_multiple_forms_on_one_message_are_all_named(self):
        extra = ('BO_ 2147483906 Both: 12 ECU\n SG_ sel M : 0|8@1+ (1,0) [0|0] "" Controller\n'
                 ' SG_ a m0 : 8|8@1+ (1,0) [0|0] "" Controller\n')
        result = run(extra)
        (item,) = [i for i in result.report["diagnostics"] if i["code"] == "unsupported_message"]
        for form in ("extended_id", "length_over_8", "multiplex"):
            self.assertIn(form, item["reason"])


if __name__ == "__main__":
    unittest.main()
