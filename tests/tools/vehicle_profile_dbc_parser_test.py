"""Accepted and rejected vectors for the core DBC parser and source inventory."""

from fractions import Fraction
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.bit_coordinates import occupied_bits
from vehicle_profiles.dbc_lexer import LexerError, LexerLimits, tokenize
from vehicle_profiles.dbc_parser import ParserLimits, parse_tokens

LEX = LexerLimits(max_sources=4, max_source_bytes=1 << 20, max_token_bytes=256)
LIMITS = ParserLimits(max_records=64, max_fields=64)
HEADER = 'VERSION "synthetic-a-v1"\nNS_ :\n    CM_\n    BA_\nBS_:\nBU_: ECU Controller\n'


def parse(text, source_id="demo-a", bus="body", limits=LIMITS):
    return parse_tokens(tokenize(text, source_id, LEX), source_id, bus, limits)


def failure(text, **kwargs):
    try:
        parse(text, **kwargs)
    except LexerError as error:
        return error
    raise AssertionError("expected failure")


def digest(preimage):
    raw = json.dumps(preimage, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


class InventoryTests(unittest.TestCase):
    def test_synthetic_source_a(self):
        inv = parse(HEADER + 'BO_ 256 Engine: 8 ECU\n'
                    ' SG_ rpm : 0|16@1+ (0.25,0) [0|16383.75] "rpm" Controller\n'
                    ' SG_ BrakePressure : 16|16@1+ (0.1,0) [0|6553.5] "kPa" Controller\n')
        self.assertEqual(inv.version, "synthetic-a-v1")
        self.assertEqual(inv.nodes, ("ECU", "Controller"))
        self.assertEqual(inv.namespace, ("CM_", "BA_"))
        (message,) = inv.messages
        self.assertEqual((message.raw_id, message.wire_id, message.identifier_format,
                          message.length, message.executable),
                         (256, 256, "standard", 8, True))
        rpm = message.signals[0]
        self.assertEqual((rpm.start, rpm.length, rpm.byte_order, rpm.signed),
                         (0, 16, "intel", False))
        self.assertEqual((rpm.scale, rpm.offset, rpm.minimum, rpm.maximum),
                         (Fraction(1, 4), 0, 0, Fraction(65535, 4)))
        for value in (rpm.scale, rpm.offset, rpm.minimum, rpm.maximum):
            self.assertIs(type(value), Fraction)
        self.assertEqual((rpm.unit, rpm.receivers), ("rpm", ("Controller",)))
        self.assertEqual(rpm.spans[0].line, 8)

    def test_exact_decimals_do_not_use_float(self):
        inv = parse('BO_ 1 M: 8 ECU\n SG_ a : 0|8@1- (0.1,-0.2) [-0.3|.5] "" X\n')
        s = inv.messages[0].signals[0]
        self.assertEqual((s.scale, s.offset, s.minimum, s.maximum),
                         (Fraction(1, 10), Fraction(-1, 5), Fraction(-3, 10), Fraction(1, 2)))
        self.assertTrue(s.signed)

    def test_bs_with_values_and_empty(self):
        self.assertIsNotNone(parse("BS_: 500 : 1, 2\n"))
        self.assertEqual(parse("").messages, ())


class IdentifierTests(unittest.TestCase):
    def msg(self, raw, length=8):
        return parse(f"BO_ {raw} M: {length} ECU\n").messages[0]

    def test_standard_bounds(self):
        self.assertEqual(self.msg(0x7FF).wire_id, 0x7FF)
        self.assertEqual(failure("BO_ 2048 M: 8 ECU\n").code, "dbc_syntax_error")

    def test_extended_bit_31(self):
        m = self.msg(0x80000000 | 0x1FFFFFFF)
        self.assertEqual((m.identifier_format, m.wire_id, m.executable),
                         ("extended", 0x1FFFFFFF, False))
        self.assertEqual(self.msg(0x80000000).wire_id, 0)
        self.assertEqual(failure(f"BO_ {0x80000000 | 0x20000000} M: 8 ECU\n").code,
                         "dbc_syntax_error")
        self.assertEqual(failure(f"BO_ {1 << 32} M: 8 ECU\n").code, "dbc_syntax_error")

    def test_length_bounds(self):
        self.assertEqual(self.msg(1, 0).length, 0)
        long = self.msg(1, 64)
        self.assertEqual((long.length, long.executable), (64, False))
        self.assertTrue(self.msg(1, 8).executable)
        self.assertEqual(failure("BO_ 1 M: 65 ECU\n").code, "dbc_syntax_error")
        self.assertEqual(failure("BO_ 1 M: -1 ECU\n").code, "dbc_syntax_error")
        self.assertEqual(failure("BO_ -1 M: 8 ECU\n").code, "dbc_syntax_error")

    def test_golden_preimages(self):
        inv = parse('BO_ 256 Engine: 8 ECU\n SG_ rpm : 0|16@1+ (1,0) [0|0] "" X\n')
        m = inv.messages[0]
        self.assertEqual(m.message_id, "dbc-message-v1:" + digest(
            ["dbc-message-id-v1", "demo-a", "body", "standard", 256, "Engine"]))
        self.assertEqual(m.signals[0].field_id, "dbc-field-v1:" + digest(
            ["dbc-field-id-v1", "demo-a", "body", "standard", 256, "rpm"]))
        # Literal serialization guards the exact preimage bytes.
        self.assertEqual(m.message_id, "dbc-message-v1:" + hashlib.sha256(
            b'["dbc-message-id-v1","demo-a","body","standard",256,"Engine"]').hexdigest())
        self.assertEqual(m.signals[0].field_id, "dbc-field-v1:" + hashlib.sha256(
            b'["dbc-field-id-v1","demo-a","body","standard",256,"rpm"]').hexdigest())

    def test_ids_depend_on_source_bus_and_format(self):
        base = parse("BO_ 1 M: 8 ECU\n").messages[0].message_id
        self.assertNotEqual(base, parse("BO_ 1 M: 8 ECU\n", source_id="demo-b").messages[0].message_id)
        self.assertNotEqual(base, parse("BO_ 1 M: 8 ECU\n", bus="other").messages[0].message_id)
        self.assertNotEqual(base, parse(f"BO_ {0x80000001} M: 8 ECU\n").messages[0].message_id)

    def test_delimiter_collisions_are_impossible(self):
        a = parse("BO_ 1 a_b: 8 ECU\n", source_id="x").messages[0].message_id
        b = parse("BO_ 1 b: 8 ECU\n", source_id="x_a").messages[0].message_id
        self.assertNotEqual(a, b)

    def test_repeated_equivalent_records_share_id_and_keep_spans(self):
        text = ('BO_ 5 M: 8 ECU\n SG_ a : 0|8@1+ (1,0) [0|0] "" X\n'
                'BO_ 5 M: 8 ECU\n SG_ a : 0|8@1+ (1,0) [0|0] "" X\n')
        inv = parse(text)
        self.assertEqual(len(inv.messages), 1)
        m = inv.messages[0]
        self.assertEqual([s.line for s in m.spans], [1, 3])
        self.assertEqual(len(m.signals), 1)
        self.assertEqual([s.line for s in m.signals[0].spans], [2, 4])

    def test_conflicting_repeats_fail(self):
        self.assertEqual(failure("BO_ 5 M: 8 ECU\nBO_ 5 M: 4 ECU\n").code, "source_conflict")
        text = ('BO_ 5 M: 8 ECU\n SG_ a : 0|8@1+ (1,0) [0|0] "" X\n'
                'BO_ 5 M: 8 ECU\n SG_ a : 0|8@1+ (2,0) [0|0] "" X\n')
        self.assertEqual(failure(text).code, "source_conflict")

    def test_same_wire_id_different_name_is_distinct(self):
        inv = parse("BO_ 5 A: 8 ECU\nBO_ 5 B: 8 ECU\n")
        self.assertEqual(len({m.message_id for m in inv.messages}), 2)


class SignalTests(unittest.TestCase):
    def sig(self, start, length, order, sign, dlc=8):
        text = f'BO_ 1 M: {dlc} ECU\n SG_ s : {start}|{length}@{order}{sign} (1,0) [0|0] "" X\n'
        return parse(text).messages[0].signals[0]

    def test_occupied_bits_come_from_shared_helper(self):
        for start, length, order, name in ((0, 16, 1, "intel"), (7, 12, 0, "motorola"),
                                           (23, 20, 0, "motorola"), (56, 8, 1, "intel")):
            s = self.sig(start, length, order, "+")
            self.assertEqual(s.occupied_bits, occupied_bits(start, length, name))

    def test_cross_byte_motorola(self):
        s = self.sig(3, 10, 0, "+")
        self.assertEqual(s.occupied_bits, (3, 2, 1, 0, 15, 14, 13, 12, 11, 10))

    def test_64_bit_signed_and_unsigned(self):
        u = self.sig(0, 64, 1, "+")
        i = self.sig(0, 64, 1, "-")
        self.assertEqual((u.length, u.signed, i.signed), (64, False, True))
        self.assertEqual(len(u.occupied_bits), 64)
        m = self.sig(7, 64, 0, "+")
        self.assertEqual(m.occupied_bits, occupied_bits(7, 64, "motorola"))

    def test_one_bit(self):
        self.assertEqual(self.sig(63, 1, 1, "+", dlc=8).occupied_bits, (63,))

    def test_out_of_payload_rejected(self):
        for args in ((57, 8, 1, "+"), (0, 65, 1, "+"), (0, 0, 1, "+"), (63, 2, 1, "+")):
            text = f'BO_ 1 M: 8 ECU\n SG_ s : {args[0]}|{args[1]}@{args[2]}{args[3]} (1,0) [0|0] "" X\n'
            self.assertEqual(failure(text).code, "dbc_syntax_error", args)
        # Motorola walk leaving the payload.
        self.assertEqual(failure('BO_ 1 M: 1 ECU\n SG_ s : 3|10@0+ (1,0) [0|0] "" X\n').code,
                         "dbc_syntax_error")

    def test_signal_in_longer_message_is_recorded(self):
        text = 'BO_ 1 M: 16 ECU\n SG_ s : 64|8@1+ (1,0) [0|0] "" X\n'
        m = parse(text).messages[0]
        self.assertFalse(m.executable)
        self.assertEqual(m.signals[0].occupied_bits, tuple(range(64, 72)))

    def test_rejections(self):
        pre = "BO_ 1 M: 8 ECU\n"
        for body in (' SG_ s : 0|8@2+ (1,0) [0|0] "" X',
                     ' SG_ s : 0|8@1* (1,0) [0|0] "" X',
                     ' SG_ s : 0|8@1+ (0,0) [0|0] "" X',
                     ' SG_ s : 0|8@1+ (1,0) [5|1] "" X',
                     ' SG_ s : 0|8@1+ (1,0) [0|0] "" ',
                     ' SG_ s : 0|8@1+ (1,0) [0|0] unit X',
                     ' SG_ s : 0|8@1+ (1,0) [0|0] "" X Y',
                     ' SG_ s : 0|8@1+ (1,0) [0|0] "" X,',
                     ' SG_ s : 0|8@1+ (1,0) [0|0 "" X',
                     ' SG_ s : -1|8@1+ (1,0) [0|0] "" X',
                     ' SG_ s 0|8@1+ (1,0) [0|0] "" X',
                     ' SG_ s : 0|8@1+ (1,0)'):
            error = failure(pre + body + "\n")
            self.assertEqual(error.code, "dbc_syntax_error", body)
            self.assertIsNotNone(error.span, body)

    def test_multiplex_is_not_supported_yet(self):
        error = failure('BO_ 1 M: 8 ECU\n SG_ s M : 0|8@1+ (1,0) [0|0] "" X\n')
        self.assertEqual(error.code, "unsupported_dbc_construct")
        self.assertEqual(error.span.line, 2)

    def test_signal_without_message(self):
        self.assertEqual(failure('SG_ s : 0|8@1+ (1,0) [0|0] "" X\n').code, "dbc_syntax_error")

    def test_signal_after_other_record_is_orphan(self):
        text = 'BO_ 1 M: 8 ECU\nBU_: A\n SG_ s : 0|8@1+ (1,0) [0|0] "" X\n'
        self.assertEqual(failure(text).code, "dbc_syntax_error")


VAL_BASE = ('BO_ 256 M: 16 ECU\n SG_ s : 0|8@1+ (1,0) [0|0] "" X\n'
            ' SG_ w : 8|64@1- (1,0) [0|0] "" X\n')


class ValueTableTests(unittest.TestCase):
    def table(self, line, signal="s"):
        inv = parse(VAL_BASE + line)
        s = {x.name: x for x in inv.messages[0].signals}[signal]
        return s.value_table

    def test_resolves_and_keeps_labels(self):
        vt = self.table('VAL_ 256 s 0 "Off" 1 "On" ;\n')
        self.assertEqual([(v.raw, v.label) for v in vt], [(0, "Off"), (1, "On")])
        self.assertEqual(vt[0].spans[0].line, 4)

    def test_64_bit_exact_integers(self):
        vt = self.table('VAL_ 256 w -9223372036854775808 "min" 9223372036854775807 "max" ;\n', "w")
        self.assertEqual([v.raw for v in vt], [-(1 << 63), (1 << 63) - 1])
        vt = self.table('VAL_ 256 s 18446744073709551615 "umax" ;\n')
        self.assertEqual(vt[0].raw, (1 << 64) - 1)
        self.assertIs(type(vt[0].raw), int)

    def test_out_of_64_bit_window_rejected(self):
        for raw in (1 << 64, -(1 << 63) - 1):
            self.assertEqual(failure(VAL_BASE + f'VAL_ 256 s {raw} "x" ;\n').code,
                             "dbc_syntax_error")

    def test_forward_reference_resolves(self):
        inv = parse('VAL_ 256 s 1 "On" ;\n' + VAL_BASE)
        self.assertEqual(inv.messages[0].signals[0].value_table[0].label, "On")

    def test_unresolved_references_fail(self):
        self.assertEqual(failure(VAL_BASE + 'VAL_ 999 s 1 "x" ;\n').code, "dbc_syntax_error")
        self.assertEqual(failure(VAL_BASE + 'VAL_ 256 nope 1 "x" ;\n').code, "dbc_syntax_error")

    def test_conflicting_labels_fail_with_span(self):
        error = failure(VAL_BASE + 'VAL_ 256 s 1 "On" ;\nVAL_ 256 s 1 "Other" ;\n')
        self.assertEqual(error.code, "source_conflict")
        self.assertIn(error.span.line, (4, 5))
        error = failure(VAL_BASE + 'VAL_ 256 s 1 "On" 1 "Other" ;\n')
        self.assertEqual(error.code, "source_conflict")

    def test_repeated_equal_labels_merge_spans(self):
        vt = self.table('VAL_ 256 s 1 "On" ;\nVAL_ 256 s 1 "On" ;\n')
        self.assertEqual(len(vt), 1)
        self.assertEqual([s.line for s in vt[0].spans], [4, 5])

    def test_malformed(self):
        for line in ('VAL_ 256 s 1 "On"\n', 'VAL_ 256 s 1 ;\n', 'VAL_ 256 s "On" 1 ;\n',
                     'VAL_ 256 s 1.5 "On" ;\n', 'VAL_ 256 s ;\n', 'VAL_ s 1 "On" ;\n',
                     'VAL_ 256 s 1 "On" ; junk\n'):
            self.assertEqual(failure(VAL_BASE + line).code, "dbc_syntax_error", line)


class RejectionTests(unittest.TestCase):
    def test_trailing_tokens_and_unknown_keywords(self):
        for text in ("BO_ 1 M: 8 ECU extra\n", 'VERSION "a" "b"\n', "BS_: 1\n",
                     "BOGUS_ 1\n", "123\n", "NS_ : x\n", 'VERSION 1\n', "BU_ A\n",
                     "BO_ 1 M 8 ECU\n", "BO_ 1 M: 8\n", "BO_ x M: 8 ECU\n", "BO_ 1.5 M: 8 ECU\n"):
            error = failure(text)
            self.assertEqual(error.code, "dbc_syntax_error", text)
            self.assertIsNotNone(error.span, text)

    def test_unsupported_families_fail_with_span(self):
        # D1 rows 66-67 families (and the rows-65/66 forms T5/T6 own) still fail.
        for keyword in ("SIG_VALTYPE_", "EV_", "SG_MUL_VAL_", "ENVVAR_DATA_", "EV_DATA_",
                        "SGTYPE_", "SGTYPE_VAL_", "SIG_TYPE_REF_", "SIGTYPE_VALTYPE_",
                        "BA_DEF_REL_", "BA_REL_", "BA_DEF_DEF_REL_", "BU_SG_REL_",
                        "BU_EV_REL_", "BU_BO_REL_", "BA_DEF_SGTYPE_", "BA_SGTYPE_"):
            error = failure(f"BO_ 1 M: 8 ECU\n{keyword} 1 2 ;\n")
            self.assertEqual(error.code, "unsupported_dbc_construct", keyword)
            self.assertEqual(error.span.line, 2)

    def test_metadata_keywords_are_no_longer_unsupported(self):
        # D1 rows 63-64: these families parse; a malformed record is a syntax error.
        for keyword in ("CM_", "BA_DEF_", "BA_DEF_DEF_", "BA_", "SIG_GROUP_", "VAL_TABLE_",
                        "BO_TX_BU_"):
            error = failure(f"BO_ 1 M: 8 ECU\n{keyword} 1 2 ;\n")
            self.assertEqual(error.code, "dbc_syntax_error", keyword)
            self.assertEqual(error.span.line, 2, keyword)


FULL_NS = """NS_ :
    NS_DESC_ CM_ BA_DEF_ BA_ VAL_ CAT_DEF_ CAT_ FILTER BA_DEF_DEF_ EV_DATA_
    ENVVAR_DATA_ SGTYPE_ SGTYPE_VAL_ BA_DEF_SGTYPE_ BA_SGTYPE_ SIG_TYPE_REF_
    VAL_TABLE_ SIG_GROUP_ SIG_VALTYPE_ SIGTYPE_VALTYPE_ BO_TX_BU_ BA_DEF_REL_
    BA_REL_ BA_DEF_DEF_REL_ BU_SG_REL_ BU_EV_REL_ BU_BO_REL_ SG_MUL_VAL_
BS_:
"""
META_BASE = ('BU_: ECU Controller\nBO_ 256 Engine: 8 ECU\n'
             ' SG_ rpm : 0|16@1+ (1,0) [0|0] "rpm" Controller\n')


class NamespaceTests(unittest.TestCase):
    """D1 row 59: NS_ entries declare vocabulary; they are not uses."""

    def test_full_vocabulary_is_accepted(self):
        inv = parse(FULL_NS + META_BASE)
        self.assertIn("EV_DATA_", inv.namespace)
        self.assertIn("SIG_VALTYPE_", inv.namespace)
        self.assertEqual(len(inv.namespace), 28)
        self.assertEqual(len(inv.messages), 1)

    def test_empty_namespace(self):
        self.assertEqual(parse("NS_ :\nBS_:\n").namespace, ())

    def test_unknown_namespace_word_fails(self):
        error = failure("NS_ :\n    CM_\n    VENDOR_X_\nBS_:\n")
        self.assertEqual(error.code, "dbc_syntax_error")
        self.assertEqual(error.span.line, 3)


class CommentTests(unittest.TestCase):
    """D1 row 63: CM_ is source metadata with no runtime behavior or binding."""

    def test_all_comment_scopes_are_retained(self):
        inv = parse(META_BASE + 'CM_ "network note";\nCM_ BU_ ECU "node note";\n'
                    'CM_ BO_ 256 "message note";\nCM_ SG_ 256 rpm "multi\nline \\"q\\"";\n')
        self.assertEqual([(c.scope, c.target, c.text) for c in inv.comments], [
            ("network", (), "network note"),
            ("node", ("ECU",), "node note"),
            ("message", (256,), "message note"),
            ("signal", (256, "rpm"), 'multi\nline "q"'),
        ])
        self.assertEqual(inv.comments[3].span.line, 7)

    def test_comments_create_no_binding_or_behavior(self):
        plain = parse(META_BASE)
        commented = parse(META_BASE + 'CM_ BO_ 256 "x";\nCM_ SG_ 256 rpm "y";\n')
        self.assertEqual(plain.messages, commented.messages)

    def test_malformed_comments_fail_with_span(self):
        for line in ('CM_ "x"\n', 'CM_ ;\n', 'CM_ BO_ "x";\n', 'CM_ SG_ 256 "x";\n',
                     'CM_ BU_ 5 "x";\n', 'CM_ BO_ 256 rpm "x";\n', 'CM_ "a" "b";\n',
                     'CM_ BO_ x "x";\n', 'CM_ VAL_ 1 "x";\n'):
            error = failure(META_BASE + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 4, line)

    def test_environment_comment_is_unsupported(self):
        error = failure(META_BASE + 'CM_ EV_ envVar "x";\n')
        self.assertEqual(error.code, "unsupported_dbc_construct")
        self.assertEqual(error.span.line, 4)


class MetadataRecordTests(unittest.TestCase):
    """D1 row 63: BO_TX_BU_, SIG_GROUP_ and standalone VAL_TABLE_ are metadata."""

    def test_transmitters(self):
        inv = parse(META_BASE + "BO_TX_BU_ 256 : ECU,Controller;\n")
        (tx,) = inv.transmitters
        self.assertEqual((tx.raw_id, tx.nodes, tx.span.line), (256, ("ECU", "Controller"), 4))
        self.assertEqual(inv.messages, parse(META_BASE).messages)

    def test_signal_group(self):
        inv = parse(META_BASE + "SIG_GROUP_ 256 Grp 1 : rpm;\n")
        (group,) = inv.signal_groups
        self.assertEqual((group.raw_id, group.name, group.repetitions, group.signals),
                         (256, "Grp", 1, ("rpm",)))
        self.assertEqual(inv.messages, parse(META_BASE).messages)

    def test_standalone_value_table(self):
        inv = parse(META_BASE + 'VAL_TABLE_ Gear -1 "R" 0 "N" 18446744073709551615 "max" ;\n')
        (table,) = inv.value_tables
        self.assertEqual(table.name, "Gear")
        self.assertEqual([(v.raw, v.label) for v in table.values],
                         [(-1, "R"), (0, "N"), ((1 << 64) - 1, "max")])
        self.assertEqual(inv.messages[0].signals[0].value_table, ())

    def test_empty_value_table_is_allowed(self):
        self.assertEqual(parse('VAL_TABLE_ Empty ;\n').value_tables[0].values, ())

    def test_malformed_metadata_records(self):
        for line in ("BO_TX_BU_ 256 ECU;\n", "BO_TX_BU_ 256 : ;\n", "BO_TX_BU_ 256 : ECU,;\n",
                     "BO_TX_BU_ 256 : ECU\n", "SIG_GROUP_ 256 Grp : rpm;\n",
                     "SIG_GROUP_ 256 Grp 1 rpm;\n", "SIG_GROUP_ 256 Grp 1 : ;\n",
                     "SIG_GROUP_ 256 Grp -1 : rpm;\n", 'VAL_TABLE_ T 1 ;\n',
                     'VAL_TABLE_ T 1.5 "x" ;\n', 'VAL_TABLE_ 1 "x" ;\n',
                     'VAL_TABLE_ T 1 "x"\n', f'VAL_TABLE_ T {1 << 64} "x" ;\n'):
            error = failure(META_BASE + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 4, line)

    def test_conflicting_standalone_labels_fail(self):
        error = failure(META_BASE + 'VAL_TABLE_ T 1 "a" 1 "b" ;\n')
        self.assertEqual(error.code, "source_conflict")


ATTR_DEFS = ('BA_DEF_ "NetNote" STRING ;\n'
             'BA_DEF_ BU_ "NodeLayer" INT 0 10 ;\n'
             'BA_DEF_ BO_ "GenMsgCycleTime" INT 0 65535 ;\n'
             'BA_DEF_ BO_ "GenMsgSendType" ENUM "Cyclic","NoMsgSendType" ;\n'
             'BA_DEF_ SG_ "GenSigStartValue" FLOAT -1e3 1e3 ;\n'
             'BA_DEF_ SG_ "SigMask" HEX 0 255 ;\n')


class AttributeTests(unittest.TestCase):
    """D1 row 64: definitions, defaults and scoped values; allowlist only."""

    def attrs(self, extra):
        return parse(META_BASE + ATTR_DEFS + extra)

    def test_definitions_defaults_and_scoped_values_are_retained(self):
        inv = self.attrs('BA_DEF_DEF_ "NetNote" "";\nBA_DEF_DEF_ "GenMsgSendType" "Cyclic";\n'
                         'BA_DEF_DEF_ "GenSigStartValue" .5;\n'
                         'BA_ "NetNote" "hello";\nBA_ "NodeLayer" BU_ ECU 3;\n'
                         'BA_ "GenMsgSendType" BO_ 256 1;\n'
                         'BA_ "GenSigStartValue" SG_ 256 rpm -2.5;\nBA_ "SigMask" SG_ 256 rpm 15;\n')
        defs = {d.name: d for d in inv.attribute_definitions}
        self.assertEqual(defs["NetNote"].scope, "network")
        self.assertEqual((defs["NodeLayer"].scope, defs["NodeLayer"].value_type,
                          defs["NodeLayer"].minimum, defs["NodeLayer"].maximum),
                         ("node", "INT", 0, 10))
        self.assertEqual(defs["GenMsgSendType"].choices, ("Cyclic", "NoMsgSendType"))
        self.assertEqual(defs["GenSigStartValue"].minimum, Fraction(-1000))
        self.assertEqual(defs["GenSigStartValue"].default, Fraction(1, 2))
        self.assertIs(type(defs["GenSigStartValue"].default), Fraction)
        self.assertEqual(defs["GenMsgSendType"].default, "Cyclic")
        values = {(a.name, a.target): a for a in inv.attributes}
        self.assertEqual(values[("NetNote", ())].value, "hello")
        self.assertEqual(values[("NodeLayer", ("ECU",))].value, 3)
        # Enum ordinals resolve through this source's declared choices.
        self.assertEqual(values[("GenMsgSendType", (256,))].value, "NoMsgSendType")
        self.assertEqual(values[("GenSigStartValue", (256, "rpm"))].value, Fraction(-5, 2))
        self.assertEqual(values[("SigMask", (256, "rpm"))].scope, "signal")
        self.assertEqual(values[("SigMask", (256, "rpm"))].span.line, 17)

    def test_retained_attributes_do_not_execute(self):
        inv = self.attrs('BA_ "GenMsgSendType" BO_ 256 0;\nBA_ "GenSigStartValue" SG_ 256 rpm 7;\n')
        self.assertEqual(inv.messages[0].signals, parse(META_BASE).messages[0].signals)
        self.assertIsNone(inv.messages[0].cycle_time_ms)

    def test_cycle_time_is_recorded_from_value_or_default(self):
        inv = self.attrs('BA_ "GenMsgCycleTime" BO_ 256 100;\n')
        self.assertEqual(inv.messages[0].cycle_time_ms, 100)
        inv = self.attrs('BA_DEF_DEF_ "GenMsgCycleTime" 20;\n')
        self.assertEqual(inv.messages[0].cycle_time_ms, 20)

    def test_cycle_time_must_be_nonnegative_integer(self):
        for line in ('BA_ "GenMsgCycleTime" BO_ 256 -1;\n', 'BA_ "GenMsgCycleTime" BO_ 256 1.5;\n'):
            error = failure(META_BASE + 'BA_DEF_ BO_ "GenMsgCycleTime" INT -10 10 ;\n' + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 5, line)
        error = failure(META_BASE + 'BA_DEF_ BO_ "GenMsgCycleTime" FLOAT 0 10 ;\n'
                        'BA_ "GenMsgCycleTime" BO_ 256 2.5;\n')
        self.assertEqual(error.code, "dbc_syntax_error")

    def test_undefined_attribute_fails_with_span(self):
        for line in ('BA_ "Nope" BO_ 256 1;\n', 'BA_DEF_DEF_ "Nope" 1;\n'):
            error = failure(META_BASE + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 4, line)

    def test_invalid_scope_fails_with_span(self):
        for line in ('BA_ "NodeLayer" BO_ 256 1;\n', 'BA_ "GenMsgCycleTime" 1;\n',
                     'BA_ "NetNote" BU_ ECU "x";\n', 'BA_ "SigMask" BO_ 256 1;\n',
                     'BA_ "GenMsgCycleTime" BO_ 999 1;\n', 'BA_ "NodeLayer" BU_ Ghost 1;\n',
                     'BA_ "SigMask" SG_ 256 nope 1;\n'):
            error = failure(META_BASE + ATTR_DEFS + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 10, line)

    def test_invalid_value_types_fail_with_span(self):
        for line in ('BA_ "NetNote" 5;\n', 'BA_ "NodeLayer" BU_ ECU "3";\n',
                     'BA_ "NodeLayer" BU_ ECU 1.5;\n', 'BA_ "GenMsgSendType" BO_ 256 2;\n',
                     'BA_ "GenMsgSendType" BO_ 256 -1;\n', 'BA_ "GenMsgSendType" BO_ 256 "Other";\n',
                     'BA_ "GenSigStartValue" SG_ 256 rpm "x";\n', 'BA_DEF_DEF_ "NetNote" 1;\n',
                     'BA_DEF_DEF_ "GenMsgSendType" "Other";\n'):
            error = failure(META_BASE + ATTR_DEFS + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 10, line)

    def test_malformed_definitions_fail(self):
        for line in ('BA_DEF_ BO_ "X" BOGUS ;\n', 'BA_DEF_ BO_ "X" INT 0 ;\n',
                     'BA_DEF_ BO_ "X" INT 0 1.5 ;\n', 'BA_DEF_ BO_ "X" INT 5 1 ;\n',
                     'BA_DEF_ BO_ "X" ENUM ;\n', 'BA_DEF_ BO_ "X" ENUM "a", ;\n',
                     'BA_DEF_ BO_ "X" ENUM "a","a" ;\n', 'BA_DEF_ BO_ X INT 0 1 ;\n',
                     'BA_DEF_ BO_ "bad name" INT 0 1 ;\n', 'BA_DEF_ VAL_ "X" INT 0 1 ;\n',
                     'BA_DEF_ BO_ "X" STRING\n', 'BA_ "NetNote" "a" junk;\n'):
            error = failure(META_BASE + 'BA_DEF_ "NetNote" STRING ;\n' + line)
            self.assertEqual(error.code, "dbc_syntax_error", line)
            self.assertEqual(error.span.line, 5, line)

    def test_conflicting_definitions_and_defaults(self):
        error = failure(META_BASE + 'BA_DEF_ BO_ "X" INT 0 1 ;\nBA_DEF_ SG_ "X" INT 0 1 ;\n')
        self.assertEqual((error.code, error.span.line), ("source_conflict", 5))
        error = failure(META_BASE + 'BA_DEF_ BO_ "X" INT 0 1 ;\n'
                        'BA_DEF_DEF_ "X" 0;\nBA_DEF_DEF_ "X" 1;\n')
        self.assertEqual((error.code, error.span.line), ("source_conflict", 6))
        inv = parse(META_BASE + 'BA_DEF_ BO_ "X" INT 0 1 ;\nBA_DEF_ BO_ "X" INT 0 1 ;\n')
        self.assertEqual([s.line for s in inv.attribute_definitions[0].spans], [4, 5])

    def test_environment_scope_is_unsupported(self):
        for line in ('BA_DEF_ EV_ "X" INT 0 1 ;\n', 'BA_ "NetNote" EV_ envVar "x";\n'):
            error = failure(META_BASE + 'BA_DEF_ "NetNote" STRING ;\n' + line)
            self.assertEqual(error.code, "unsupported_dbc_construct", line)
            self.assertEqual(error.span.line, 5, line)


FRAME_DEF = ('BA_DEF_ BO_ "VFrameFormat" ENUM "ExtendedCAN","StandardCAN","J1939PG",'
             '"StandardCAN_FD","ExtendedCAN_FD" ;\n')


class FrameFormatAndProtocolTests(unittest.TestCase):
    """D1 allowlist: VFrameFormat via declared labels, BusType/ProtocolType = CAN."""

    def test_frame_format_uses_declared_labels_not_vendor_ordinals(self):
        # Ordinal 1 is StandardCAN in this source's declaration.
        inv = parse(META_BASE + FRAME_DEF + 'BA_ "VFrameFormat" BO_ 256 1;\n')
        self.assertEqual(inv.messages[0].frame_format, "StandardCAN")
        self.assertTrue(inv.messages[0].executable)
        inv = parse(META_BASE + FRAME_DEF + 'BA_DEF_DEF_ "VFrameFormat" "StandardCAN";\n')
        self.assertEqual(inv.messages[0].frame_format, "StandardCAN")

    def test_frame_format_contradictions_fail(self):
        for line in ('BA_ "VFrameFormat" BO_ 256 0;\n',  # ExtendedCAN on a standard id
                     'BA_ "VFrameFormat" BO_ 256 "ExtendedCAN_FD";\n',
                     'BA_DEF_DEF_ "VFrameFormat" "ExtendedCAN";\n'):
            error = failure(META_BASE + FRAME_DEF + line)
            self.assertEqual(error.code, "source_conflict", line)
            self.assertIsNotNone(error.span, line)
        error = failure(f'BO_ {0x80000001} X: 8 ECU\n' + FRAME_DEF +
                        f'BA_ "VFrameFormat" BO_ {0x80000001} 1;\n')
        self.assertEqual(error.code, "source_conflict")
        error = failure('BO_ 1 Long: 16 ECU\n' + FRAME_DEF + 'BA_ "VFrameFormat" BO_ 1 1;\n')
        self.assertEqual(error.code, "source_conflict")

    def test_fd_frame_is_inventory_only(self):
        inv = parse('BO_ 1 M: 8 ECU\nBO_ 2 L: 64 ECU\n' + FRAME_DEF +
                    'BA_ "VFrameFormat" BO_ 1 3;\nBA_ "VFrameFormat" BO_ 2 "StandardCAN_FD";\n')
        self.assertEqual([(m.frame_format, m.executable) for m in inv.messages],
                         [("StandardCAN_FD", False), ("StandardCAN_FD", False)])

    def test_unknown_frame_format_label_is_unsupported(self):
        error = failure(META_BASE + FRAME_DEF + 'BA_ "VFrameFormat" BO_ 256 2;\n')
        self.assertEqual((error.code, error.span.line), ("unsupported_dbc_construct", 5))

    def test_bus_and_protocol_type(self):
        for defs in ('BA_DEF_ "BusType" STRING ;\nBA_DEF_ "ProtocolType" STRING ;\n',):
            inv = parse(META_BASE + defs + 'BA_DEF_DEF_ "BusType" "";\nBA_ "BusType" "CAN";\n'
                        'BA_ "ProtocolType" "CAN";\n')
            self.assertEqual(len(inv.attributes), 2)
            self.assertEqual(len(parse(META_BASE + defs).attribute_definitions), 2)
            for line in ('BA_ "BusType" "LIN";\n', 'BA_ "ProtocolType" "J1939";\n',
                         'BA_DEF_DEF_ "BusType" "CAN FD";\n'):
                error = failure(META_BASE + defs + line)
                self.assertEqual((error.code, error.span.line),
                                 ("unsupported_dbc_construct", 6), line)
        error = failure(META_BASE + 'BA_DEF_ BO_ "ProtocolType" STRING ;\n'
                        'BA_ "ProtocolType" BO_ 256 "J1939";\n')
        self.assertEqual((error.code, error.span.line), ("unsupported_dbc_construct", 5))

    def test_non_selected_bus_still_fails(self):
        error = failure("BOGUS_ 1\n", bus="not-selected", source_id="other")
        self.assertEqual(error.span.source_id, "other")

    def test_resource_limits(self):
        text = "".join(f"BO_ {n} M{n}: 8 ECU\n" for n in range(5))
        with self.assertRaises(LexerError) as ctx:
            parse(text, limits=ParserLimits(max_records=4, max_fields=64))
        self.assertEqual(ctx.exception.code, "resource_overflow")
        text = "BO_ 1 M: 8 ECU\n" + "".join(
            f' SG_ s{n} : {n}|1@1+ (1,0) [0|0] "" X\n' for n in range(3))
        with self.assertRaises(LexerError) as ctx:
            parse(text, limits=ParserLimits(max_records=64, max_fields=2))
        self.assertEqual(ctx.exception.code, "resource_overflow")
        for bad in (-1, 1.5):
            with self.assertRaises(ValueError):
                ParserLimits(max_records=bad, max_fields=1)


if __name__ == "__main__":
    unittest.main()
