"""Source acquisition and exact-token vectors for the synthetic DBC lexer."""

from fractions import Fraction
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from vehicle_profiles.dbc_lexer import (LexerError, LexerLimits, SourceRequest,
                                        load_sources, parse_decimal, tokenize)

LIMITS = LexerLimits(max_sources=4, max_source_bytes=4096, max_token_bytes=64)


def lex(text, limits=LIMITS, source_id="demo-a"):
    return tokenize(text, source_id, limits)


def failure(callable_, *args):
    try:
        callable_(*args)
    except LexerError as error:
        return error
    raise AssertionError("expected LexerError")


class DecimalTests(unittest.TestCase):
    def test_accepted_spellings_are_exact(self):
        for text, value in ((".5", Fraction(1, 2)), ("+01", 1), ("1.", 1),
                            ("-1.25e+2", -125), ("0.250", Fraction(1, 4)),
                            ("1E2", 100), ("-0", 0)):
            self.assertEqual(parse_decimal(text), Fraction(value), text)
            self.assertIs(type(parse_decimal(text)), Fraction)

    def test_no_binary_float_rounding(self):
        self.assertEqual(parse_decimal("0.1") + parse_decimal("0.2"), Fraction(3, 10))

    def test_rejected_spellings(self):
        for text in (".", "1e+", "--1", "1_0", "0x10", "NaN", "Inf", "", " 1", "1 ",
                     "+", "1e", "1.5.2", "١"):
            error = failure(parse_decimal, text)
            self.assertEqual(error.code, "dbc_syntax_error", text)

    def test_exponent_bound_is_resource_overflow(self):
        self.assertEqual(failure(parse_decimal, "1e99999999").code, "resource_overflow")

    def test_lexer_rejects_same_spellings_in_context(self):
        for text in (".", "1e+", "--1", "1_0", "0x10", "1.5.2", "+-1"):
            error = failure(lex, "BO_ " + text)
            self.assertEqual(error.code, "dbc_syntax_error", text)

    def test_lexer_values(self):
        tokens = lex(".5 +01 1. -1.25e+2 7")
        self.assertEqual([t.kind for t in tokens],
                         ["decimal", "int", "decimal", "decimal", "int"])
        self.assertEqual([t.value for t in tokens],
                         [Fraction(1, 2), 1, 1, -125, 7])
        self.assertEqual(tokens[1].text, "+01")


class TokenTests(unittest.TestCase):
    def test_signal_line_tokens_and_spans(self):
        tokens = lex(' SG_ rpm : 0|16@1+ (0.25,-40) [0|1] "rpm" Controller')
        self.assertEqual([t.text for t in tokens],
                         ["SG_", "rpm", ":", "0", "|", "16", "@", "1", "+", "(",
                          "0.25", ",", "-40", ")", "[", "0", "|", "1", "]", '"rpm"',
                          "Controller"])
        self.assertEqual(tokens[0].kind, "word")
        self.assertEqual(tokens[8].kind, "punct")
        self.assertEqual(tokens[-2].kind, "string")
        self.assertEqual(tokens[-2].value, "rpm")
        first = tokens[0].span
        self.assertEqual((first.source_id, first.line, first.column,
                          first.end_line, first.end_column), ("demo-a", 1, 2, 1, 5))

    def test_spans_across_line_endings(self):
        for newline in ("\n", "\r\n", "\r"):
            tokens = lex("A" + newline + "  B" + newline + "C")
            self.assertEqual([(t.span.line, t.span.column) for t in tokens],
                             [(1, 1), (2, 3), (3, 1)], repr(newline))

    def test_comments_end_at_line_ending(self):
        tokens = lex('A // x "unterminated\nB // tail')
        self.assertEqual([t.text for t in tokens], ["A", "B"])
        self.assertEqual(tokens[1].span.line, 2)

    def test_comment_marker_inside_string_is_data(self):
        self.assertEqual(lex('"a // b"')[0].value, "a // b")

    def test_string_escapes_and_multiline(self):
        tokens = lex('"q\\" b\\\\ \n tail"')
        self.assertEqual(tokens[0].value, 'q" b\\ \n tail')
        self.assertEqual((tokens[0].span.line, tokens[0].span.end_line), (1, 2))

    def test_bad_escape_and_unterminated_string(self):
        for text in ('"a\\n"', '"a\\', '"abc', '"abc\\"'):
            error = failure(lex, "X " + text)
            self.assertEqual(error.code, "dbc_syntax_error", text)
            self.assertEqual((error.span.source_id, error.span.line), ("demo-a", 1))

    def test_unknown_characters_fail_with_span(self):
        for text in ("A\n  #", "A / B", "A $", "A é", "﻿A", "A\x00"):
            error = failure(lex, text)
            self.assertEqual(error.code, "dbc_syntax_error", repr(text))
            self.assertIsNotNone(error.span)
        self.assertEqual(failure(lex, "A\n  #").span.column, 3)

    def test_word_and_number_boundaries(self):
        self.assertEqual([t.text for t in lex("BO_ 256 Engine_1:")],
                         ["BO_", "256", "Engine_1", ":"])
        self.assertEqual(failure(lex, "9abc").code, "dbc_syntax_error")

    def test_token_byte_limit_is_overflow_not_truncation(self):
        limits = LexerLimits(4, 4096, 5)
        self.assertEqual([t.text for t in lex("abcde", limits)], ["abcde"])
        for text in ("abcdef", '"abcd"', "1234567", '"ééé"'):
            error = failure(lex, text, limits)
            self.assertEqual(error.code, "resource_overflow", text)
            self.assertEqual(error.span.line, 1)

    def test_oversized_integer_overflow_carries_span(self):
        limits = LexerLimits(4, 8192, 8192)
        error = failure(lex, "9" * 5000, limits)
        self.assertEqual(error.code, "resource_overflow")
        self.assertIsNotNone(error.span)
        self.assertEqual(error.span.line, 1)

    def test_overlimit_decimal_token_fails_with_span(self):
        error = failure(lex, "1.2345678", LexerLimits(4, 4096, 5))
        self.assertEqual(error.code, "resource_overflow")
        self.assertIsNotNone(error.span)

    def test_integer_with_many_leading_zeros_is_int(self):
        limits = LexerLimits(4, 8192, 8192)
        token = lex("0" * 4999 + "1", limits)[0]
        self.assertEqual(token.kind, "int")
        self.assertIs(type(token.value), int)
        self.assertEqual(token.value, 1)


class SourceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.dir = Path(directory.name)

    def request(self, data, **overrides):
        path = self.dir / "input.dbc"
        path.write_bytes(data)
        fields = dict(source_id="demo-a", path=str(path), logical_path="pkg/demo-a.dbc",
                      bus="body", revision=None,
                      license_ref="synthetic:Apache-2.0", encoding="utf-8")
        fields.update(overrides)
        return SourceRequest(**fields)

    def test_load_records_digest_over_original_bytes(self):
        data = b'VERSION "v"\r\nNS_ :\r\n'
        [record] = load_sources([self.request(data)], LIMITS)
        self.assertEqual(record.sha256, hashlib.sha256(data).hexdigest())
        self.assertEqual(record.byte_length, len(data))
        self.assertEqual([t.value for t in record.tokens], ["VERSION", "v", "NS_", ":"])
        self.assertEqual((record.source_id, record.bus, record.logical_path,
                          record.revision, record.license_ref, record.encoding),
                         ("demo-a", "body", "pkg/demo-a.dbc", None,
                          "synthetic:Apache-2.0", "utf-8"))

    def test_expected_digest_match_and_mismatch(self):
        data = b"A"
        good = hashlib.sha256(data).hexdigest()
        load_sources([self.request(data, expected_sha256=good)], LIMITS)
        error = failure(load_sources,
                        [self.request(data, expected_sha256="0" * 64)], LIMITS)
        self.assertEqual(error.code, "source_digest_mismatch")
        self.assertEqual((error.span.source_id, error.span.line, error.span.column),
                         ("demo-a", 1, 1))

    def test_digest_is_checked_before_decode(self):
        error = failure(load_sources,
                        [self.request(b"\xff", expected_sha256="0" * 64)], LIMITS)
        self.assertEqual(error.code, "source_digest_mismatch")

    def test_strict_decode_failures_have_spans(self):
        error = failure(load_sources, [self.request(b"A\nB \xff")], LIMITS)
        self.assertEqual(error.code, "source_decode")
        self.assertEqual((error.span.line, error.span.column), (2, 3))
        error = failure(load_sources,
                        [self.request(b"A\x81", encoding="cp1252")], LIMITS)
        self.assertEqual(error.code, "source_decode")
        self.assertEqual((error.span.line, error.span.column), (1, 2))

    def test_declared_encoding_changes_interpretation(self):
        data = b'"caf\xe9"'
        [record] = load_sources([self.request(data, encoding="cp1252")], LIMITS)
        self.assertEqual(record.tokens[0].value, "café")
        self.assertEqual(failure(load_sources, [self.request(data)], LIMITS).code,
                         "source_decode")

    def test_unknown_encoding_is_rejected(self):
        for encoding in ("latin-1", "utf-8-sig", "guess", None):
            error = failure(load_sources, [self.request(b"A", encoding=encoding)], LIMITS)
            self.assertEqual(error.code, "source_decode", str(encoding))

    def test_provenance_is_required(self):
        for value in ("", "  ", None):
            error = failure(load_sources, [self.request(b"A", license_ref=value)], LIMITS)
            self.assertEqual(error.code, "source_provenance_missing", str(value))

    def test_missing_file_is_source_io_with_host_only_path(self):
        request = self.request(b"A")
        Path(request.path).unlink()
        error = failure(load_sources, [request], LIMITS)
        self.assertEqual(error.code, "source_io")
        self.assertIn(request.path, error.host_detail)
        self.assertNotIn(request.path, str(error))
        self.assertNotIn(request.path, repr(error.span))

    def test_limits_fail_as_overflow(self):
        small = LexerLimits(max_sources=1, max_source_bytes=3, max_token_bytes=64)
        self.assertEqual(len(load_sources([self.request(b"ABC")], small)), 1)
        error = failure(load_sources, [self.request(b"ABCD")], small)
        self.assertEqual(error.code, "resource_overflow")
        self.assertEqual(error.span.source_id, "demo-a")
        two = [self.request(b"A"), self.request(b"B", source_id="demo-b")]
        self.assertEqual(failure(load_sources, two, small).code, "resource_overflow")
        none = LexerLimits(0, 3, 64)
        self.assertEqual(failure(load_sources, [self.request(b"A")], none).code,
                         "resource_overflow")

    def test_duplicate_source_ids_are_rejected(self):
        with self.assertRaises(ValueError):
            load_sources([self.request(b"A"), self.request(b"B")], LIMITS)

    def test_invalid_limits_are_rejected(self):
        for args in ((-1, 1, 1), (1, True, 1), (1, 1, 1.5)):
            with self.assertRaises(ValueError):
                LexerLimits(*args)

    def test_returned_records_hold_no_absolute_path(self):
        request = self.request(b'A "b"')
        [record] = load_sources([request], LIMITS)
        self.assertNotIn(request.path, repr(record))
        self.assertNotIn(str(self.dir), repr(record))


if __name__ == "__main__":
    unittest.main()
