"""Compose parsed DBC sources into one message set keyed by wire identity (host tools only).

Messages are keyed by `(bus, identifier_format, wire_id)` and compared before any
unsupported or unrecognized message is dropped, so filtering cannot hide a
contradiction. Identical mechanics deduplicate, nonoverlapping fields compose,
and every conflict is a fatal `source_conflict` naming both definitions. Input
order never selects a winner: representatives are chosen by stable id order.
"""

from dataclasses import dataclass, replace

from .dbc_lexer import Span
from .dbc_parser import Message, Signal, SourceInventory


class ComposeError(Exception):
    """Fatal composition error carrying the spans of both conflicting definitions."""

    def __init__(self, reason: str, spans: tuple[Span, ...]):
        super().__init__(f"source_conflict: {reason}")
        self.code, self.reason, self.spans = "source_conflict", reason, spans


@dataclass(frozen=True)
class ComposedSignal(Signal):
    """A representative field plus every original name and contributing source."""
    names: tuple[str, ...] = ()
    sources: tuple[str, ...] = ()


@dataclass(frozen=True)
class Alias:
    """One original message or field definition behind a composed representative."""
    kind: str  # message or field
    representative: str
    source_id: str
    name: str
    original_id: str
    spans: tuple[Span, ...]


def _span_key(span: Span) -> tuple:
    return (span.source_id, span.line, span.column, span.end_line, span.end_column)


def _spans(spans) -> tuple[Span, ...]:
    return tuple(sorted(set(spans), key=_span_key))


def _signature(signal: Signal) -> tuple:
    return (signal.start, signal.length, signal.byte_order, signal.signed, signal.scale,
            signal.offset, signal.minimum, signal.maximum, signal.unit,
            tuple((entry.raw, entry.label) for entry in signal.value_table))


def _signal_source(signal: Signal) -> str:
    return signal.spans[0].source_id


def _conflict_reason(first: Signal, second: Signal) -> str | None:
    bits = set(first.occupied_bits) & set(second.occupied_bits)
    if bits:
        if set(first.occupied_bits) != set(second.occupied_bits):
            return "fields partially overlap in occupied bits"
        labels = {entry.raw: entry.label for entry in first.value_table}
        for entry in second.value_table:
            if labels.get(entry.raw, entry.label) != entry.label:
                return f"conflicting labels for raw value {entry.raw}"
        return "fields have different mechanics on the same bits"
    if first.name == second.name:
        return f"incompatible definitions of source signal name {first.name}"
    return None


def _compose_fields(signals: list[Signal]) -> tuple[tuple[ComposedSignal, ...], list[Alias]]:
    # Occurrences with one field id (a repeated message in one source) share spans.
    by_id: dict[str, list[Signal]] = {}
    for signal in signals:
        by_id.setdefault(signal.field_id, []).append(signal)
    groups: dict[tuple, list[Signal]] = {}
    for field_id, found in sorted(by_id.items()):
        for other in found[1:]:
            if _signature(other) != _signature(found[0]):
                raise ComposeError(
                    f"incompatible definitions of source signal name {found[0].name}",
                    (found[0].spans[0], other.spans[0]))
        merged = replace(found[0], spans=_spans(s for item in found for s in item.spans))
        groups.setdefault(_signature(merged), []).append(merged)
    ordered = sorted(groups.values(), key=lambda group: group[0].field_id)
    for index, later in enumerate(ordered):
        for earlier in ordered[:index]:
            for first in earlier:
                for second in later:
                    reason = _conflict_reason(first, second)
                    if reason:
                        raise ComposeError(reason, (first.spans[0], second.spans[0]))
    composed, aliases = [], []
    for group in ordered:
        rep = group[0]
        composed.append(ComposedSignal(
            **{name: getattr(rep, name) for name in Signal.__dataclass_fields__
               if name != "spans"},
            spans=_spans(s for item in group for s in item.spans),
            names=tuple(sorted({item.name for item in group})),
            sources=tuple(sorted({_signal_source(item) for item in group}))))
        aliases += [Alias("field", rep.field_id, _signal_source(item), item.name, item.field_id,
                          item.spans) for item in group]
    return tuple(composed), aliases


def compose(inventories: list[SourceInventory]) -> tuple[tuple[Message, ...], tuple[Alias, ...]]:
    """Return composed messages in key order and the alias inventory."""
    keyed: dict[tuple, list[Message]] = {}
    for inventory in inventories:
        for message in inventory.messages:
            key = (inventory.bus, message.identifier_format, message.wire_id)
            keyed.setdefault(key, []).append(message)
    messages, aliases = [], []
    for key, fragments in sorted(keyed.items()):
        fragments.sort(key=lambda item: item.message_id)
        rep = fragments[0]
        for other in fragments[1:]:
            if other.length != rep.length:
                raise ComposeError("messages with one wire identity have different payload "
                                   "lengths", (rep.spans[0], other.spans[0]))
        fields, field_aliases = _compose_fields([s for item in fragments for s in item.signals])
        messages.append(replace(rep, signals=fields,
                                executable=all(item.executable for item in fragments),
                                unsupported_forms=tuple(sorted(
                                    {f for item in fragments for f in item.unsupported_forms})),
                                spans=_spans(s for item in fragments for s in item.spans)))
        aliases += [Alias("message", rep.message_id, item.spans[0].source_id, item.name,
                          item.message_id, item.spans) for item in fragments]
        aliases += field_aliases
    return tuple(messages), tuple(aliases)
