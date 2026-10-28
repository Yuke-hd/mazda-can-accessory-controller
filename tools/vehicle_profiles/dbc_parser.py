"""Core DBC parser: tokens to a source inventory for the version-one subset.

Handles VERSION, NS_, BS_, BU_, BO_, SG_ (including multiplex markers) and VAL_,
plus the metadata records CM_, BO_TX_BU_, SIG_GROUP_, VAL_TABLE_, BA_DEF_,
BA_DEF_DEF_, BA_, SIG_VALTYPE_ and SG_MUL_VAL_. Multiplexed and IEEE-typed
messages are inventoried but never executable.
Metadata creates no bindings; only the allowlisted attributes GenMsgCycleTime,
VFrameFormat, BusType and ProtocolType are interpreted. Every other record
fails with a stable code and span. Numbers stay exact; bit coordinates come
only from `bit_coordinates.occupied_bits`.
"""

from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json
import re

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
SUPPORTED = frozenset({"VERSION", "NS_", "BS_", "BU_", "BO_", "SG_", "VAL_", "CM_",
                       "BO_TX_BU_", "SIG_GROUP_", "VAL_TABLE_", "BA_DEF_", "BA_DEF_DEF_",
                       "BA_", "SIG_VALTYPE_", "SG_MUL_VAL_"})
# Known record families not parsed yet; they fail rather than being skipped.
UNSUPPORTED = (NS_KEYWORDS | {"EV_"}) - SUPPORTED
RECORD_KEYWORDS = SUPPORTED | UNSUPPORTED

# Object keywords inside CM_/BA_DEF_/BA_ mapped to inventory scopes.
SCOPES = {"BU_": "node", "BO_": "message", "SG_": "signal"}
ATTRIBUTE_TYPES = frozenset({"INT", "HEX", "FLOAT", "STRING", "ENUM"})
FRAME_FORMATS = {"StandardCAN": ("standard", False), "ExtendedCAN": ("extended", False),
                 "StandardCAN_FD": ("standard", True), "ExtendedCAN_FD": ("extended", True)}
_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_MULTIPLEX_MARKER = re.compile(r"M|m[0-9]+M?")
VALUE_TYPE_FORMS = {1: "ieee_float", 2: "ieee_double"}


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
    multiplex: str | None = None  # source marker: M, mN or mNM
    value_type: int = 0  # SIG_VALTYPE_ code: 0 integer, 1 IEEE float, 2 IEEE double


@dataclass(frozen=True)
class Message:
    message_id: str
    raw_id: int
    wire_id: int
    identifier_format: str  # standard or extended
    name: str
    length: int
    sender: str
    executable: bool  # standard id, length <= 8 and not a CAN FD frame format only
    signals: tuple[Signal, ...]
    spans: tuple[Span, ...]
    cycle_time_ms: int | None = None  # source-declared GenMsgCycleTime; timing record only
    frame_format: str | None = None  # declared VFrameFormat label
    unsupported_forms: tuple[str, ...] = ()  # reported wire forms that block execution


@dataclass(frozen=True)
class Comment:
    scope: str  # network, node, message or signal
    target: tuple  # (), (node,), (raw_id,) or (raw_id, signal)
    text: str
    span: Span


@dataclass(frozen=True)
class Transmitters:
    raw_id: int
    nodes: tuple[str, ...]
    span: Span


@dataclass(frozen=True)
class SignalGroup:
    raw_id: int
    name: str
    repetitions: int
    signals: tuple[str, ...]
    span: Span


@dataclass(frozen=True)
class ValueTable:
    name: str
    values: tuple[ValueLabel, ...]
    span: Span


@dataclass(frozen=True)
class AttributeDefinition:
    name: str
    scope: str  # network, node, message or signal
    value_type: str  # INT, HEX, FLOAT, STRING or ENUM
    minimum: int | Fraction | None
    maximum: int | Fraction | None
    choices: tuple[str, ...]
    default: object  # None when no BA_DEF_DEF_; enum defaults hold the label
    spans: tuple[Span, ...]


@dataclass(frozen=True)
class AttributeValue:
    name: str
    scope: str
    target: tuple
    value: object  # int, Fraction or str; enum values hold the declared label
    spans: tuple[Span, ...]  # identical repeats share one entry


@dataclass(frozen=True)
class MultiplexRange:
    raw_id: int
    signal: str
    switch: str
    ranges: tuple[tuple[int, int], ...]
    span: Span


@dataclass(frozen=True)
class SourceInventory:
    source_id: str
    bus: str
    version: str | None
    namespace: tuple[str, ...]
    nodes: tuple[str, ...]
    messages: tuple[Message, ...]
    comments: tuple[Comment, ...] = ()
    transmitters: tuple[Transmitters, ...] = ()
    signal_groups: tuple[SignalGroup, ...] = ()
    value_tables: tuple[ValueTable, ...] = ()
    attribute_definitions: tuple[AttributeDefinition, ...] = ()
    attributes: tuple[AttributeValue, ...] = ()
    multiplex_ranges: tuple[MultiplexRange, ...] = ()


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
    pending_types: list[tuple[Span, int, str, int]] = []
    pending_ranges: list[tuple[Span, int, str, str, tuple]] = []
    meta = _Metadata()
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
        elif keyword == "VAL_":
            _parse_values(cur, head, pending_values)
        elif keyword == "SIG_VALTYPE_":
            _parse_value_type(cur, head, pending_types)
        elif keyword == "SG_MUL_VAL_":
            _parse_multiplex_range(cur, head, pending_ranges)
        elif keyword == "CM_":
            _parse_comment(cur, head, meta)
        elif keyword == "BO_TX_BU_":
            _parse_transmitters(cur, head, meta)
        elif keyword == "SIG_GROUP_":
            _parse_signal_group(cur, head, meta)
        elif keyword == "VAL_TABLE_":
            _parse_value_table(cur, head, meta)
        elif keyword == "BA_DEF_":
            _parse_attribute_definition(cur, head, meta)
        elif keyword == "BA_DEF_DEF_":
            _parse_attribute_default(cur, head, meta)
        else:
            _parse_attribute_value(cur, head, meta)

    _resolve_values(pending_values, messages)
    definitions = _resolve_attributes(meta, messages, nodes)
    _resolve_value_types(pending_types, messages)
    ranges = _resolve_multiplex_ranges(pending_ranges, messages)
    _mark_unsupported_forms(messages)
    return SourceInventory(source_id, bus, version, tuple(namespace), tuple(nodes),
                           tuple(_freeze_message(b) for b in messages.values()),
                           tuple(meta.comments), tuple(meta.transmitters),
                           tuple(meta.signal_groups), tuple(meta.value_tables),
                           definitions, tuple(meta.values), ranges)


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
    multiplex = None
    if marker is not None and marker.kind == "word":
        if _MULTIPLEX_MARKER.fullmatch(marker.text) is None:
            cur.fail("malformed multiplex marker", marker)
        multiplex = marker.text
        cur.pos += 1
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
                   minimum, maximum, unit.value, tuple(receivers), multiplex)
    existing = builder.signals.get(name.text)
    if existing is None:
        wire = builder.message
        builder.signals[name.text] = {
            "field_id": field_id(source_id, bus, wire["identifier_format"], wire["wire_id"],
                                 name.text),
            "name": name.text, "fingerprint": fingerprint, "bits": bits, "values": {},
            "multiplex": multiplex, "value_type": 0, "spans": [span]}
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


def _parse_value_type(cur: _Cursor, head: Token, pending: list) -> None:
    ident = _message_raw_id(cur)
    name = cur.word("signal name")
    cur.punct(":")
    code = cur.integer("signal value type")
    span = _join(head.span, cur.punct(";").span)
    if code.value not in (0, 1, 2):
        _syntax(cur, "SIG_VALTYPE_ code must be 0, 1 or 2", code.span)
    pending.append((span, ident, name.text, code.value))


def _parse_multiplex_range(cur: _Cursor, head: Token, pending: list) -> None:
    ident = _message_raw_id(cur)
    name = cur.word("signal name")
    switch = cur.word("multiplexer signal name")
    ranges = []
    while True:
        low = cur.integer("range start")
        high = cur.integer("range end")  # the lexer reads "0-3" as 0 and -3
        adjacent = (high.span.line, high.span.column) == (low.span.end_line, low.span.end_column)
        if not low.text.isdigit() or not high.text.startswith("-") or not adjacent:
            _syntax(cur, "multiplex range must be written start-end", _join(low.span, high.span))
        if -high.value < low.value:
            _syntax(cur, "multiplex range start exceeds end", _join(low.span, high.span))
        ranges.append((low.value, -high.value))
        if not cur.is_punct(","):
            break
        cur.punct(",")
    span = _join(head.span, cur.punct(";").span)
    pending.append((span, ident, name.text, switch.text, tuple(ranges)))


def _signals_of(messages: dict, ident: int, name: str, span: Span, what: str) -> list:
    targets = [b.signals[name] for (raw, _), b in messages.items()
               if raw == ident and name in b.signals]
    if not targets:
        _syntax(None, f"{what} references an unknown message or signal", span)
    return targets


def _resolve_value_types(pending: list, messages: dict) -> None:
    for span, ident, name, code in pending:
        for target in _signals_of(messages, ident, name, span, "SIG_VALTYPE_"):
            if target.get("typed") and target["value_type"] != code:
                raise ParserError("source_conflict", "conflicting SIG_VALTYPE_ codes", span)
            target["value_type"], target["typed"] = code, True


def _resolve_multiplex_ranges(pending: list, messages: dict) -> tuple[MultiplexRange, ...]:
    out = []
    for span, ident, name, switch, ranges in pending:
        _signals_of(messages, ident, name, span, "SG_MUL_VAL_")
        for (raw, _), builder in messages.items():
            if raw == ident and name in builder.signals:
                marker = builder.signals[name]["multiplex"]
                if name == switch or marker is None or not marker.startswith("m"):
                    _syntax(None, "SG_MUL_VAL_ target is not a multiplexed signal", span)
                selector = builder.signals.get(switch)
                if selector is None or not (selector["multiplex"] or "").endswith("M"):
                    _syntax(None, "SG_MUL_VAL_ switch is not a multiplexer signal", span)
        out.append(MultiplexRange(ident, name, switch, ranges, span))
    return tuple(out)


def _mark_unsupported_forms(messages: dict) -> None:
    """Multiplexed or IEEE-typed messages stay in the report and are never executable."""
    for builder in messages.values():
        forms = []
        if any(sig["multiplex"] for sig in builder.signals.values()):
            forms.append("multiplex")
        for code, form in VALUE_TYPE_FORMS.items():
            if any(sig["value_type"] == code for sig in builder.signals.values()):
                forms.append(form)
        if forms:
            builder.message["unsupported_forms"] = forms
            builder.message["executable"] = False


class _Metadata:
    def __init__(self):
        self.comments: list[Comment] = []
        self.transmitters: list[Transmitters] = []
        self.signal_groups: list[SignalGroup] = []
        self.value_tables: list[ValueTable] = []
        self.definitions: dict[str, dict] = {}
        self.defaults: dict[str, tuple[Token, Span]] = {}
        self.pending: list[tuple[Token, str, tuple, Token, Span]] = []
        self.values: list[AttributeValue] = []


def _end(cur: _Cursor, head: Token) -> Span:
    return _join(head.span, cur.punct(";").span)


def _scope_keyword(cur: _Cursor) -> str | None:
    """Consume an optional BU_/BO_/SG_ object keyword; EV_ is an unsupported family."""
    token = cur.peek()
    if token is None or token.kind != "word":
        return None
    if token.text == "EV_":
        cur.fail("environment-variable scope is not supported", token,
                 "unsupported_dbc_construct")
    if token.text not in SCOPES:
        cur.fail("expected BU_, BO_, SG_ or a quoted string", token)
    cur.pos += 1
    return SCOPES[token.text]


def _message_raw_id(cur: _Cursor) -> int:
    raw = cur.integer("message id")
    if raw.value < 0 or raw.value >= (1 << 32):
        _syntax(cur, "message id out of range", raw.span)
    return raw.value


def _target(cur: _Cursor, scope: str | None) -> tuple:
    if scope is None:
        return ()
    if scope == "node":
        return (cur.word("node name").text,)
    raw = _message_raw_id(cur)
    if scope == "message":
        return (raw,)
    return (raw, cur.word("signal name").text)


def _parse_comment(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    scope = _scope_keyword(cur)
    target = _target(cur, scope)
    text = cur.string("comment string")
    meta.comments.append(Comment(scope or "network", target, text.value, _end(cur, head)))


def _parse_transmitters(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    raw = _message_raw_id(cur)
    cur.punct(":")
    nodes = [cur.word("transmitter node").text]
    while cur.is_punct(","):
        cur.punct(",")
        nodes.append(cur.word("transmitter node").text)
    meta.transmitters.append(Transmitters(raw, tuple(nodes), _end(cur, head)))


def _parse_signal_group(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    raw = _message_raw_id(cur)
    name = cur.word("signal group name")
    repetitions = cur.integer("repetitions")
    if repetitions.value < 0:
        _syntax(cur, "repetitions must be nonnegative", repetitions.span)
    cur.punct(":")
    signals = [cur.word("signal name").text]
    while cur.more() and cur.peek().kind == "word":
        signals.append(cur.take("signal name").text)
    meta.signal_groups.append(SignalGroup(raw, name.text, repetitions.value, tuple(signals),
                                          _end(cur, head)))


def _parse_value_table(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    name = cur.word("value table name")
    entries: dict[int, ValueLabel] = {}
    while not cur.is_punct(";"):
        raw = cur.integer("raw value")
        label = cur.string("value label")
        if not RAW_MIN <= raw.value <= RAW_MAX:
            _syntax(cur, "raw value outside the 64-bit integer range", raw.span)
        pair_span = _join(raw.span, label.span)
        existing = entries.get(raw.value)
        if existing is not None and existing.label != label.value:
            raise ParserError("source_conflict", "conflicting labels for one raw value", pair_span)
        spans = (existing.spans if existing else ()) + (pair_span,)
        entries[raw.value] = ValueLabel(raw.value, label.value, spans)
    meta.value_tables.append(ValueTable(name.text, tuple(entries.values()), _end(cur, head)))


def _attribute_name(cur: _Cursor) -> Token:
    name = cur.string("attribute name")
    if _WORD.fullmatch(name.value) is None:
        cur.fail("attribute name must be a DBC identifier", name)
    return name


def _parse_attribute_definition(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    scope = _scope_keyword(cur) or "network"
    name = _attribute_name(cur)
    kind = cur.word("attribute value type")
    if kind.text not in ATTRIBUTE_TYPES:
        cur.fail("unknown attribute value type", kind)
    minimum = maximum = None
    choices: list[str] = []
    if kind.text in ("INT", "HEX"):
        minimum, maximum = cur.integer("minimum").value, cur.integer("maximum").value
    elif kind.text == "FLOAT":
        minimum, maximum = cur.number("minimum"), cur.number("maximum")
    elif kind.text == "ENUM":
        choices.append(cur.string("enum label").value)
        while cur.is_punct(","):
            cur.punct(",")
            label = cur.string("enum label")
            if label.value in choices:
                cur.fail("duplicate enum label", label)
            choices.append(label.value)
    span = _end(cur, head)
    if minimum is not None and minimum > maximum:
        _syntax(cur, "attribute minimum exceeds maximum", span)
    shape = (scope, kind.text, minimum, maximum, tuple(choices))
    existing = meta.definitions.get(name.value)
    if existing is None:
        meta.definitions[name.value] = {"shape": shape, "spans": [span]}
    elif existing["shape"] != shape:
        raise ParserError("source_conflict", "attribute redefined differently", span)
    else:
        existing["spans"].append(span)


def _attribute_value_token(cur: _Cursor) -> Token:
    token = cur.take("attribute value")
    if token.kind not in ("int", "decimal", "string"):
        cur.fail("expected an attribute value", token)
    return token


def _parse_attribute_default(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    name = _attribute_name(cur)
    value = _attribute_value_token(cur)
    span = _end(cur, head)
    existing = meta.defaults.get(name.value)
    if existing is not None and (existing[0].kind, existing[0].value) != (value.kind, value.value):
        raise ParserError("source_conflict", "attribute default redefined differently", span)
    if existing is None:
        meta.defaults[name.value] = (value, span)


def _parse_attribute_value(cur: _Cursor, head: Token, meta: _Metadata) -> None:
    name = _attribute_name(cur)
    scope = _scope_keyword(cur) or "network"
    target = _target(cur, None if scope == "network" else scope)
    value = _attribute_value_token(cur)
    meta.pending.append((name, scope, target, value, _end(cur, head)))


def _typed_value(shape: tuple, token: Token, span: Span):
    """Check a value token against its definition; enum ordinals use declared choices."""
    _, kind, _, _, choices = shape
    if kind in ("INT", "HEX") and token.kind == "int":
        return token.value
    if kind == "FLOAT" and token.kind in ("int", "decimal"):
        return Fraction(token.value)
    if kind == "STRING" and token.kind == "string":
        return token.value
    if kind == "ENUM":
        if token.kind == "int" and 0 <= token.value < len(choices):
            return choices[token.value]
        if token.kind == "string" and token.value in choices:
            return token.value
    raise ParserError("dbc_syntax_error", "attribute value does not match its definition", span)


def _resolve_attributes(meta: _Metadata, messages: dict, nodes: list[str]):
    definitions = []
    defaults: dict[str, tuple[object, Span]] = {}
    for name, (token, span) in meta.defaults.items():
        if name not in meta.definitions:
            raise ParserError("dbc_syntax_error", "default for an undefined attribute", span)
        defaults[name] = (_typed_value(meta.definitions[name]["shape"], token, span), span)
    for name, item in meta.definitions.items():
        scope, kind, minimum, maximum, choices = item["shape"]
        definitions.append(AttributeDefinition(name, scope, kind, minimum, maximum, choices,
                                               defaults.get(name, (None,))[0],
                                               tuple(item["spans"])))
    explicit: dict[tuple[str, tuple], tuple[object, Span]] = {}
    spans: dict[tuple[str, tuple], list[Span]] = {}
    order: list[tuple[tuple[str, tuple], str]] = []
    for name, scope, target, token, span in meta.pending:
        definition = meta.definitions.get(name.value)
        if definition is None:
            raise ParserError("dbc_syntax_error", "value for an undefined attribute", span)
        if definition["shape"][0] != scope:
            raise ParserError("dbc_syntax_error", "attribute value has the wrong object scope", span)
        if not _target_exists(scope, target, messages, nodes):
            raise ParserError("dbc_syntax_error", "attribute references an unknown object", span)
        value = _typed_value(definition["shape"], token, span)
        key = (name.value, target)
        if key in explicit:
            # Input order never supplies precedence: repeats must agree exactly.
            if explicit[key][0] != value:
                raise ParserError("source_conflict",
                                  "repeated attribute value differs for one object", span)
            spans[key].append(span)
            continue
        explicit[key], spans[key] = (value, span), [span]
        order.append((key, scope))
    meta.values.extend(AttributeValue(key[0], scope, key[1], explicit[key][0], tuple(spans[key]))
                       for key, scope in order)
    _apply_allowlist(meta, defaults, explicit, messages, nodes)
    return tuple(definitions)


def _target_exists(scope: str, target: tuple, messages: dict, nodes: list[str]) -> bool:
    if scope == "network":
        return True
    if scope == "node":
        return target[0] in nodes
    matches = [b for (raw, _), b in messages.items() if raw == target[0]]
    if scope == "message":
        return bool(matches)
    return any(target[1] in b.signals for b in matches)


def _effective(name: str, target: tuple, defaults: dict, explicit: dict):
    """The explicit value for one object, else the default, as (value, span) or None."""
    return explicit.get((name, target)) or defaults.get(name)


def _objects(scope: str, messages: dict, nodes: list[str]) -> list[tuple]:
    if scope == "network":
        return [()]
    if scope == "node":
        return [(node,) for node in nodes]
    if scope == "message":
        return sorted({(raw,) for raw, _ in messages})
    return sorted({(raw, signal) for (raw, _), b in messages.items() for signal in b.signals})


def _apply_allowlist(meta: _Metadata, defaults: dict, explicit: dict, messages: dict,
                     nodes: list[str]) -> None:
    """Interpret only D1's allowlisted attributes; everything else stays report data."""
    for name in ("BusType", "ProtocolType"):
        definition = meta.definitions.get(name)
        if definition is None:
            continue
        scope = definition["shape"][0]
        for target in _objects(scope, messages, nodes):
            found = _effective(name, target, defaults, explicit)
            # An empty string is absent metadata; only CAN is accepted otherwise.
            if found is not None and found[0] not in ("", "CAN"):
                raise ParserError("unsupported_dbc_construct",
                                  f"{name} {found[0]!r} is not CAN", found[1])
    for name in ("GenMsgCycleTime", "VFrameFormat"):
        definition = meta.definitions.get(name)
        if definition is None:
            continue
        scope, kind = definition["shape"][:2]
        span = definition["spans"][0]
        if scope != "message" or (name == "VFrameFormat" and kind != "ENUM"):
            raise ParserError("unsupported_dbc_construct",
                              f"{name} must be a message attribute"
                              + (" with declared enum labels" if name == "VFrameFormat" else ""),
                              span)
        for (raw, _), builder in messages.items():
            found = _effective(name, (raw,), defaults, explicit)
            if found is None:
                continue
            value, value_span = found
            if name == "GenMsgCycleTime":
                if type(value) is not int or value < 0:
                    raise ParserError("dbc_syntax_error",
                                      "GenMsgCycleTime must be a nonnegative integer", value_span)
                builder.message["cycle_time_ms"] = value
                continue
            if value not in FRAME_FORMATS:
                raise ParserError("unsupported_dbc_construct",
                                  f"unsupported VFrameFormat label {value!r}", value_span)
            identifier_format, fd = FRAME_FORMATS[value]
            message = builder.message
            if identifier_format != message["identifier_format"] or (
                    not fd and message["length"] > EXECUTABLE_LENGTH):
                raise ParserError("source_conflict",
                                  "VFrameFormat contradicts the message id or length", value_span)
            message["frame_format"] = value
            if fd:
                message["executable"] = False


def _freeze_message(builder: _MessageBuilder) -> Message:
    signals = []
    for item in builder.signals.values():
        (start, length, order, signed, scale, offset, minimum, maximum, unit, receivers,
         multiplex) = item["fingerprint"]
        table = tuple(ValueLabel(raw, entry["label"], tuple(entry["spans"]))
                      for raw, entry in item["values"].items())
        signals.append(Signal(item["field_id"], item["name"], start, length, order, signed,
                              scale, offset, minimum, maximum, unit, receivers, item["bits"],
                              table, tuple(item["spans"]), multiplex, item["value_type"]))
    values = dict(builder.message)
    values["spans"] = tuple(values["spans"])
    values["unsupported_forms"] = tuple(values.get("unsupported_forms", ()))
    return Message(signals=tuple(signals), **values)
