"""Strict DBC source acquisition and exact-token lexer for the version-one subset."""

from bisect import bisect_right
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import re

ENCODINGS = frozenset({"utf-8", "cp1252"})
PUNCTUATION = frozenset(":|@()[],;+-")
WHITESPACE = frozenset(" \t\r\n")
# Bounds exact-value work for hostile exponents and mantissas independent of request limits.
MAX_EXPONENT = 4096
MAX_MANTISSA_DIGITS = 4000

_DECIMAL = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
_INTEGER = re.compile(r"[+-]?[0-9]+")
_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_LINE_END = re.compile(r"\r\n|\r|\n")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_BOUNDARY = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.")


@dataclass(frozen=True)
class Span:
    """One-based line/column range; the end column is exclusive."""
    source_id: str
    line: int
    column: int
    end_line: int
    end_column: int


class LexerError(Exception):
    """Fatal source error with a stable code. `host_detail` may hold a local path."""

    def __init__(self, code: str, reason: str, span: Span | None = None,
                 host_detail: str = ""):
        super().__init__(f"{code}: {reason}")
        self.code, self.reason, self.span, self.host_detail = code, reason, span, host_detail


@dataclass(frozen=True)
class LexerLimits:
    max_sources: int
    max_source_bytes: int
    max_token_bytes: int

    def __post_init__(self):
        for name in ("max_sources", "max_source_bytes", "max_token_bytes"):
            value = getattr(self, name)
            if type(value) is not int or value < 0:
                raise ValueError(f"{name} must be a nonnegative integer")


@dataclass(frozen=True)
class SourceRequest:
    source_id: str
    path: str  # local input path; host diagnostics only
    logical_path: str
    bus: str
    revision: str | None
    license_ref: str
    encoding: str
    expected_sha256: str | None = None


@dataclass(frozen=True)
class Token:
    kind: str  # word, int, decimal, string or punct
    text: str  # original spelling, including string quotes
    value: object  # word/punct text, int, Fraction or decoded string
    span: Span


@dataclass(frozen=True)
class SourceRecord:
    source_id: str
    logical_path: str
    bus: str
    revision: str | None
    license_ref: str
    encoding: str
    sha256: str
    byte_length: int
    tokens: tuple[Token, ...]


def _whole(source_id: str) -> Span:
    return Span(source_id, 1, 1, 1, 1)


def _syntax(reason: str, span: Span | None = None) -> LexerError:
    return LexerError("dbc_syntax_error", reason, span)


def parse_decimal(text: str) -> Fraction:
    """Parse a complete source decimal token directly into an exact Fraction."""
    if not isinstance(text, str) or _DECIMAL.fullmatch(text) is None:
        raise _syntax("invalid numeric token")
    mantissa, _, exponent = text.lower().partition("e")
    sign = -1 if mantissa.startswith("-") else 1
    whole, _, fraction = mantissa.lstrip("+-").partition(".")
    digits = (whole + fraction).lstrip("0")
    if len(digits) > MAX_MANTISSA_DIGITS or len(exponent.lstrip("+-0")) > 4:
        raise LexerError("resource_overflow", "numeric token exceeds exact-value bounds")
    scale = (int(exponent) if exponent else 0) - len(fraction)
    if abs(scale) > MAX_EXPONENT:
        raise LexerError("resource_overflow", "numeric exponent exceeds exact-value bounds")
    value = Fraction(int(digits or "0")) * Fraction(10) ** scale
    return sign * value


class _Lines:
    def __init__(self, text: str):
        self.starts = [0] + [m.end() for m in _LINE_END.finditer(text)]

    def locate(self, index: int) -> tuple[int, int]:
        line = bisect_right(self.starts, index) - 1
        return line + 1, index - self.starts[line] + 1


def tokenize(text: str, source_id: str, limits: LexerLimits) -> tuple[Token, ...]:
    """Tokenize decoded DBC text; every failure carries a span, nothing is skipped."""
    lines = _Lines(text)
    tokens: list[Token] = []
    size, index = len(text), 0

    def span(start: int, end: int) -> Span:
        return Span(source_id, *lines.locate(start), *lines.locate(end))

    def check_size(start: int, end: int) -> None:
        # The limit counts UTF-8 bytes of the decoded token text.
        if len(text[start:end].encode("utf-8")) > limits.max_token_bytes:
            raise LexerError("resource_overflow", "token exceeds the request token-byte limit",
                             span(start, end))

    def emit(kind: str, start: int, end: int, value: object) -> None:
        raw = text[start:end]
        check_size(start, end)
        tokens.append(Token(kind, raw, raw if value is None else value, span(start, end)))

    while index < size:
        char = text[index]
        following = text[index + 1] if index + 1 < size else ""
        if char in WHITESPACE:
            index += 1
        elif char == "/" and following == "/":
            match = _LINE_END.search(text, index)
            index = match.start() if match else size
        elif char == '"':
            end, parts = index + 1, []
            while True:
                if end >= size:
                    raise _syntax("unterminated string", span(index, size))
                if text[end] == '"':
                    break
                if text[end] == "\\":
                    escaped = text[end + 1] if end + 1 < size else ""
                    if escaped not in ('"', "\\"):
                        raise _syntax("unsupported string escape", span(end, min(end + 2, size)))
                    parts.append(escaped)
                    end += 2
                else:
                    parts.append(text[end])
                    end += 1
            emit("string", index, end + 1, "".join(parts))
            index = end + 1
        elif char in "+-" and following in ("+", "-"):
            raise _syntax("repeated sign", span(index, index + 2))
        elif char in PUNCTUATION and not (char in "+-" and (following.isascii() and following.isdigit()
                                                            or following == ".")):
            emit("punct", index, index + 1, None)
            index += 1
        elif char in "+-." or (char.isascii() and char.isdigit()):
            match = _DECIMAL.match(text, index)
            if match is None:
                raise _syntax("invalid numeric token", span(index, index + 1))
            end = match.end()
            if end < size and text[end] in _BOUNDARY:
                raise _syntax("invalid numeric token", span(index, end + 1))
            raw = match.group()
            if _INTEGER.fullmatch(raw):
                check_size(index, end)
                digits = raw.lstrip("+-").lstrip("0")
                if len(digits) > MAX_MANTISSA_DIGITS:
                    raise LexerError("resource_overflow",
                                     "numeric token exceeds exact-value bounds", span(index, end))
                emit("int", index, end, (-1 if raw[0] == "-" else 1) * int(digits or "0"))
            else:
                try:
                    value = parse_decimal(raw)
                except LexerError as error:
                    raise LexerError(error.code, error.reason, span(index, end)) from None
                emit("decimal", index, end, value)
            index = end
        else:
            match = _WORD.match(text, index)
            if match is None:
                raise _syntax("unexpected character", span(index, index + 1))
            emit("word", index, match.end(), None)
            index = match.end()
    return tuple(tokens)


def _read(request: SourceRequest, limits: LexerLimits) -> bytes:
    try:
        with open(request.path, "rb") as stream:
            data = stream.read(limits.max_source_bytes + 1)
    except OSError as error:
        raise LexerError("source_io", "cannot read source", _whole(request.source_id),
                         f"{request.path}: {error}") from None
    if len(data) > limits.max_source_bytes:
        raise LexerError("resource_overflow", "source exceeds the request source-byte limit",
                         _whole(request.source_id))
    return data


def _decode(data: bytes, request: SourceRequest) -> str:
    try:
        return data.decode(request.encoding, errors="strict")
    except UnicodeDecodeError as error:
        # The bytes before the failure are valid, so they locate it in characters.
        prefix = data[:error.start].decode(request.encoding)
        line, column = _Lines(prefix).locate(len(prefix))
        raise LexerError("source_decode", f"invalid {request.encoding} byte sequence",
                         Span(request.source_id, line, column, line, column)) from None


def load_sources(requests: list[SourceRequest], limits: LexerLimits) -> list[SourceRecord]:
    """Read each source once, verify provenance and digest, decode strictly and tokenize."""
    ids = [request.source_id for request in requests]
    if len(set(ids)) != len(ids):
        raise ValueError("source ids must be unique")
    if len(requests) > limits.max_sources:
        raise LexerError("resource_overflow", "source count exceeds the request limit",
                         _whole(requests[limits.max_sources].source_id))
    records = []
    for request in requests:
        if request.expected_sha256 is not None and (
                not isinstance(request.expected_sha256, str)
                or _SHA256.fullmatch(request.expected_sha256) is None):
            raise ValueError("expected_sha256 must be 64 lowercase hex digits")
        if not isinstance(request.license_ref, str) or not request.license_ref.strip():
            raise LexerError("source_provenance_missing", "license/provenance reference required",
                             _whole(request.source_id))
        data = _read(request, limits)
        digest = hashlib.sha256(data).hexdigest()
        if request.expected_sha256 is not None and digest != request.expected_sha256:
            raise LexerError("source_digest_mismatch", "source bytes do not match the expected digest",
                             _whole(request.source_id))
        if request.encoding not in ENCODINGS:
            raise LexerError("source_decode", "encoding must be utf-8 or cp1252",
                             _whole(request.source_id))
        tokens = tokenize(_decode(data, request), request.source_id, limits)
        records.append(SourceRecord(request.source_id, request.logical_path, request.bus,
                                    request.revision, request.license_ref, request.encoding,
                                    digest, len(data), tokens))
    return records
