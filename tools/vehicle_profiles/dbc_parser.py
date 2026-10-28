"""Core DBC parser: tokens to a source inventory for the version-one subset.

Handles VERSION, NS_, BS_, BU_, BO_, plain SG_ and VAL_. Every other record
fails with a stable code and span. Numbers stay exact; bit coordinates come
only from `bit_coordinates.occupied_bits`.
"""

from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json

from .bit_coordinates import occupied_bits
from .dbc_lexer import LexerError, Span, Token

ParserError = LexerError

EXTENDED_FLAG = 1 << 31
MAX_STANDARD_ID = (1 << 11) - 1
MAX_EXTENDED_ID = (1 << 29) - 1
MAX_LENGTH = 64
EXECUTABLE_LENGTH = 8
RAW_MIN, RAW_MAX = -(1 << 63), (1 << 64) - 1

# Vocabulary a DBC may list inside NS_; these are declarations, not uses.
NS_KEYWORDS = frozenset("""
NS_DESC_ CM_ BA_DEF_ BA_ VAL_ CAT_DEF_ CAT_ FILTER BA_DEF_DEF_ EV_DATA_ ENVVAR_DATA_
SGTYPE_ SGTYPE_VAL_ BA_DEF_SGTYPE_ BA_SGTYPE_ SIG_TYPE_REF_ VAL_TABLE_ SIG_GROUP_
SIG_VALTYPE_ SIGTYPE_VALTYPE_ BO_TX_BU_ BA_DEF_REL_ BA_REL_ BA_DEF_DEF_REL_ BU_SG_REL_
BU_EV_REL_ BU_BO_REL_ SG_MUL_VAL_
""".split())
SUPPORTED = frozenset({"VERSION", "NS_", "BS_", "BU_", "BO_", "SG_", "VAL_"})
# Known record families not parsed yet; they fail rather than being skipped.
UNSUPPORTED = (NS_KEYWORDS | {"EV_"}) - SUPPORTED
RECORD_KEYWORDS = SUPPORTED | UNSUPPORTED


@dataclass(frozen=True)
class ParserLimits:
    max_records: int
    max_fields: int

    def __post_init__(self):
        for name in ("max_records", "max_fields"):
            value = getattr(self, name)
            if type(value) is not int or value < 0:
                raise ValueError(f"{name} must be a nonnegative integer")


@dataclass(frozen=True)
class ValueLabel:
    raw: int
    label: str
    spans: tuple[Span, ...]


@dataclass(frozen=True)
class Signal:
    field_id: str
    name: str
    start: int
    length: int
    byte_order: str  # intel or motorola
    signed: bool
    scale: Fraction
    offset: Fraction
    minimum: Fraction
    maximum: Fraction
    unit: str
    receivers: tuple[str, ...]
    occupied_bits: tuple[int, ...]
    value_table: tuple[ValueLabel, ...]
    spans: tuple[Span, ...]


@dataclass(frozen=True)
class Message:
    message_id: str
    raw_id: int
    wire_id: int
    identifier_format: str  # standard or extended
    name: str
    length: int
    sender: str
    executable: bool  # standard id and length <= 8 only
    signals: tuple[Signal, ...]
    spans: tuple[Span, ...]


@dataclass(frozen=True)
class SourceInventory:
    source_id: str
    bus: str
    version: str | None
    namespace: tuple[str, ...]
    nodes: tuple[str, ...]
    messages: tuple[Message, ...]


def _digest(preimage: list) -> str:
    raw = json.dumps(preimage, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def message_id(source_id: str, bus: str, identifier_format: str, wire_id: int, name: str) -> str:
    return "dbc-message-v1:" + _digest(
        ["dbc-message-id-v1", source_id, bus, identifier_format, wire_id, name])


def field_id(source_id: str, bus: str, identifier_format: str, wire_id: int, name: str) -> str:
    return "dbc-field-v1:" + _digest(
        ["dbc-field-id-v1", source_id, bus, identifier_format, wire_id, name])


def _join(first: Span, last: Span) -> Span:
    return Span(first.source_id, first.line, first.column, last.end_line, last.end_column)


class _Cursor:
    def __init__(self, tokens: tuple[Token, ...]):
        self.tokens, self.pos, self.first = tokens, 0, None

    def more(self) -> bool:
        return self.pos < len(self.tokens)

    def peek(self) -> Token | None:
        return self.tokens[self.pos] if self.more() else None

    def fail(self, reason: str, token: Token | None = None, code: str = "dbc_syntax_error"):
        if token is not None:
            span = token.span
        elif self.first is not None:
            span = _join(self.first.span, self.tokens[-1].span)
        else:
            span = None
        raise ParserError(code, reason, span)

    def take(self, reason: str) -> Token:
        token = self.peek()
        if token is None:
            self.fail(f"unexpected end of source; expected {reason}")
        self.pos += 1
        return token

    def punct(self, text: str) -> Token:
        token = self.take(f"'{text}'")
        if token.kind != "punct" or token.text != text:
            self.fail(f"expected '{text}'", token)
        return token

    def word(self, reason: str = "name") -> Token:
        token = self.take(reason)
        if token.kind != "word":
            self.fail(f"expected {reason}", token)
        return token

    def string(self, reason: str = "string") -> Token:
        token = self.take(reason)
        if token.kind != "string":
            self.fail(f"expected {reason}", token)
        return token

    def integer(self, reason: str) -> Token:
        token = self.take(reason)
        if token.kind != "int" or type(token.value) is not int:
            self.fail(f"expected {reason}", token)
        return token

    def number(self, reason: str) -> Fraction:
        token = self.take(reason)
        if token.kind not in ("int", "decimal"):
            self.fail(f"expected {reason}", token)
        return Fraction(token.value)

    def is_punct(self, text: str) -> bool:
        token = self.peek()
        return token is not None and token.kind == "punct" and token.text == text


class _MessageBuilder:
    def __init__(self, message: dict):
        self.message = message
        self.signals: dict[str, dict] = {}


def parse_tokens(tokens: tuple[Token, ...], source_id: str, bus: str,
                 limits: ParserLimits) -> SourceInventory:
    """Parse a complete token stream; any unsupported or malformed record is fatal."""
    cur = _Cursor(tokens)
    version: str | None = None
    namespace: list[str] = []
    nodes: list[str] = []
    messages: dict[tuple[int, str], _MessageBuilder] = {}
    pending_values: list[tuple[Span, int, str, list[tuple[Token, str]]]] = []
    current: _MessageBuilder | None = None
    records = fields = 0

    def bump_records(start: Token):
        nonlocal records
        records += 1
        if records > limits.max_records:
            cur.fail("record count exceeds the request limit", start, "resource_overflow")

    while cur.more():
        head = cur.take("record keyword")
        cur.first = head
        if head.kind != "word":
            cur.fail("expected a record keyword", head)
        keyword = head.text
        if keyword not in RECORD_KEYWORDS:
            cur.fail("unknown record keyword", head)
        if keyword in UNSUPPORTED:
            cur.fail(f"{keyword} is not supported yet", head, "unsupported_dbc_construct")
        bump_records(head)

        if keyword != "SG_":
            current = None
        if keyword == "VERSION":
            version = cur.string("version string").value
        elif keyword == "NS_":
            cur.punct(":")
            while cur.more() and cur.peek().kind == "word" and cur.peek().text in NS_KEYWORDS:
                namespace.append(cur.take("keyword").text)
        elif keyword == "BS_":
            cur.punct(":")
            if cur.more() and cur.peek().kind == "int":
                cur.integer("baud rate")
                cur.punct(":")
                cur.integer("btr1")
                cur.punct(",")
                cur.integer("btr2")
        elif keyword == "BU_":
            cur.punct(":")
            while cur.more() and cur.peek().kind == "word" and cur.peek().text not in RECORD_KEYWORDS:
                nodes.append(cur.take("node").text)
        elif keyword == "BO_":
            current = _parse_message(cur, head, source_id, bus, messages)
        elif keyword == "SG_":
            if current is None:
                cur.fail("signal outside a message", head)
            fields += 1
            if fields > limits.max_fields:
                cur.fail("field count exceeds the request limit", head, "resource_overflow")
            _parse_signal(cur, head, source_id, bus, current)
        else:
            _parse_values(cur, head, pending_values)

    _resolve_values(pending_values, messages)
    return SourceInventory(source_id, bus, version, tuple(namespace), tuple(nodes),
                           tuple(_freeze_message(b) for b in messages.values()))


def _syntax(cur: _Cursor, reason: str, span: Span):
    raise ParserError("dbc_syntax_error", reason, span)


def _parse_message(cur: _Cursor, head: Token, source_id: str, bus: str,
                   messages: dict) -> _MessageBuilder:
    raw = cur.integer("message id")
    name = cur.word("message name")
    cur.punct(":")
    length = cur.integer("payload length")
    sender = cur.word("sender")
    span = _join(head.span, sender.span)
    if raw.value < 0 or raw.value >= (1 << 32):
        _syntax(cur, "message id out of range", raw.span)
    if raw.value & EXTENDED_FLAG:
        identifier_format, wire_id = "extended", raw.value & ~EXTENDED_FLAG
        if wire_id > MAX_EXTENDED_ID:
            _syntax(cur, "extended id exceeds 29 bits", raw.span)
    else:
        identifier_format, wire_id = "standard", raw.value
        if wire_id > MAX_STANDARD_ID:
            _syntax(cur, "standard id exceeds 11 bits", raw.span)
    if not 0 <= length.value <= MAX_LENGTH:
        _syntax(cur, "payload length outside 0..64", length.span)
    key = (raw.value, name.text)
    builder = messages.get(key)
    if builder is None:
        builder = messages[key] = _MessageBuilder({
            "message_id": message_id(source_id, bus, identifier_format, wire_id, name.text),
            "raw_id": raw.value, "wire_id": wire_id, "identifier_format": identifier_format,
            "name": name.text, "length": length.value, "sender": sender.text,
            "executable": identifier_format == "standard" and length.value <= EXECUTABLE_LENGTH,
            "spans": [span]})
    elif builder.message["length"] != length.value:
        raise ParserError("source_conflict", "repeated message has a different length", span)
    else:
        builder.message["spans"].append(span)
    return builder


def _parse_signal(cur: _Cursor, head: Token, source_id: str, bus: str,
                  builder: _MessageBuilder) -> None:
    name = cur.word("signal name")
    marker = cur.peek()
    if marker is not None and marker.kind == "word":
        cur.fail("multiplexed signals are not supported yet", marker, "unsupported_dbc_construct")
    cur.punct(":")
    start = cur.integer("start bit")
    cur.punct("|")
    length = cur.integer("signal length")
    cur.punct("@")
    order = cur.integer("byte order")
    sign = cur.take("sign")
    if order.value not in (0, 1):
        cur.fail("byte order must be 0 or 1", order)
    if sign.kind != "punct" or sign.text not in ("+", "-"):
        cur.fail("sign must be + or -", sign)
    cur.punct("(")
    scale = cur.number("scale")
    cur.punct(",")
    offset = cur.number("offset")
    cur.punct(")")
    cur.punct("[")
    minimum = cur.number("minimum")
    cur.punct("|")
    maximum = cur.number("maximum")
    cur.punct("]")
    unit = cur.string("unit string")
    receivers = [cur.word("receiver").text]
    while cur.is_punct(","):
        cur.punct(",")
        receivers.append(cur.word("receiver").text)
    span = _join(head.span, cur.tokens[cur.pos - 1].span)
    if start.value < 0 or not 1 <= length.value <= MAX_LENGTH:
        cur.fail("signal start or length out of range", start if start.value < 0 else length)
    if scale == 0:
        _syntax(cur, "scale must not be zero", span)
    if minimum > maximum:
        _syntax(cur, "range minimum exceeds maximum", span)
    byte_order = "intel" if order.value == 1 else "motorola"
    bits = occupied_bits(start.value, length.value, byte_order)
    if max(bits) >= builder.message["length"] * 8:
        _syntax(cur, "signal occupies bits outside the payload", span)
    fingerprint = (start.value, length.value, byte_order, sign.text == "-", scale, offset,
                   minimum, maximum, unit.value, tuple(receivers))
    existing = builder.signals.get(name.text)
    if existing is None:
        wire = builder.message
        builder.signals[name.text] = {
            "field_id": field_id(source_id, bus, wire["identifier_format"], wire["wire_id"],
                                 name.text),
            "name": name.text, "fingerprint": fingerprint, "bits": bits, "values": {},
            "spans": [span]}
    elif existing["fingerprint"] != fingerprint:
        raise ParserError("source_conflict", "repeated signal has different mechanics", span)
    else:
        existing["spans"].append(span)


def _parse_values(cur: _Cursor, head: Token, pending: list) -> None:
    ident = cur.integer("message id")
    name = cur.word("signal name")
    pairs = []
    while not cur.is_punct(";"):
        raw = cur.integer("raw value")
        label = cur.string("value label")
        if not RAW_MIN <= raw.value <= RAW_MAX:
            _syntax(cur, "raw value outside the 64-bit integer range", raw.span)
        pairs.append((raw.value, label.value, _join(raw.span, label.span)))
    end = cur.punct(";")
    if not pairs:
        _syntax(cur, "value table has no entries", _join(head.span, end.span))
    pending.append((_join(head.span, end.span), ident.value, name.text, pairs))


def _resolve_values(pending: list, messages: dict) -> None:
    for span, ident, name, pairs in pending:
        targets = [b.signals[name] for (raw, _), b in messages.items()
                   if raw == ident and name in b.signals]
        if not targets:
            _syntax(None, "value table references an unknown message or signal", span)
        for target in targets:
            for raw, label, pair_span in pairs:
                entry = target["values"].setdefault(raw, {"label": label, "spans": []})
                if entry["label"] != label:
                    raise ParserError("source_conflict",
                                      "conflicting labels for one raw value", pair_span)
                entry["spans"].append(pair_span)


def _freeze_message(builder: _MessageBuilder) -> Message:
    signals = []
    for item in builder.signals.values():
        start, length, order, signed, scale, offset, minimum, maximum, unit, receivers = \
            item["fingerprint"]
        table = tuple(ValueLabel(raw, entry["label"], tuple(entry["spans"]))
                      for raw, entry in item["values"].items())
        signals.append(Signal(item["field_id"], item["name"], start, length, order, signed,
                              scale, offset, minimum, maximum, unit, receivers, item["bits"],
                              table, tuple(item["spans"])))
    values = dict(builder.message)
    values["spans"] = tuple(values["spans"])
    return Message(signals=tuple(signals), **values)
