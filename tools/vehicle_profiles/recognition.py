"""Reviewed recognition packs: strict JSON rules matched against a source inventory.

A rule matches only when an explicit ASCII case-insensitive alias, the exact
unit and every declared mechanical predicate all hold. Spelling similarity,
substrings and file order never select a canonical input. Pack data is never
executed (host tools only).
"""

from dataclasses import dataclass
import json
import re
from typing import Any

from .dbc_parser import Message, Signal

_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
BYTE_ORDERS = ("intel", "motorola")


def strict_json(text: str) -> Any:
    """Parse JSON, rejecting duplicate object keys and nonfinite numbers."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate key {key}")
            result[key] = value
        return result

    def invalid(value):
        raise ValueError(f"non-finite number {value}")
    try:
        return json.loads(text, object_pairs_hook=pairs, parse_constant=invalid)
    except json.JSONDecodeError as error:
        raise ValueError(f"malformed JSON: {error.msg}") from None


def exact_object(value: Any, keys: str, optional: str = "") -> dict:
    """Require a JSON object whose keys are all of `keys` plus any of `optional`."""
    required, allowed = set(keys.split()), set(keys.split()) | set(optional.split())
    if not isinstance(value, dict) or not required <= set(value) <= allowed:
        raise ValueError(f"expected an object with fields: {' '.join(sorted(allowed))}")
    return value


def text_field(value: Any, name: str, *, identifier: bool = False) -> str:
    if type(value) is not str or not value or (identifier and not _ID.match(value)):
        raise ValueError(f"{name} must be a non-empty {'identifier' if identifier else 'string'}")
    return value


@dataclass(frozen=True)
class Match:
    aliases: tuple[str, ...]  # lowercase ASCII
    unit: str
    length: int | None
    byte_order: str | None
    signed: bool | None


@dataclass(frozen=True)
class Rule:
    match: Match
    binding: dict  # reviewed binding template; validated structurally by validate_profile


@dataclass(frozen=True)
class Pack:
    pack_id: str
    revision: str
    profile_id: str
    semantic_policy_revision: str
    rules: tuple[Rule, ...]


def _rule(item: Any, index: int) -> Rule:
    rule = exact_object(item, "match binding")
    match = exact_object(rule["match"], "aliases unit", "length byte_order signed")
    aliases = match["aliases"]
    if not isinstance(aliases, list) or not aliases:
        raise ValueError(f"rules[{index}] aliases must be a non-empty array")
    lowered = []
    for alias in aliases:
        if type(alias) is not str or not _ID.match(alias) or not alias.isascii():
            raise ValueError(f"rules[{index}] aliases must be ASCII names")
        lowered.append(alias.lower())
    if len(set(lowered)) != len(lowered):
        raise ValueError(f"rules[{index}] aliases repeat case-insensitively")
    if type(match["unit"]) is not str:
        raise ValueError(f"rules[{index}] unit must be a string")
    length = match.get("length")
    if "length" in match and (type(length) is not int or not 1 <= length <= 64):
        raise ValueError(f"rules[{index}] length must be an integer in 1..64")
    order = match.get("byte_order")
    if "byte_order" in match and order not in BYTE_ORDERS:
        raise ValueError(f"rules[{index}] byte_order must be intel or motorola")
    signed = match.get("signed")
    if "signed" in match and type(signed) is not bool:
        raise ValueError(f"rules[{index}] signed must be a boolean")
    binding = exact_object(rule["binding"], "key runtime_id type unit capabilities evidence "
                           "conversion invalid_value")
    text_field(binding["key"], "binding key", identifier=True)
    if type(binding["runtime_id"]) is not int:
        raise ValueError("binding runtime_id must be an integer")
    if binding["type"] != "number":
        raise ValueError("version-one packs support number bindings only")
    if type(binding["unit"]) is not str:
        raise ValueError("binding unit must be a string")
    if not isinstance(binding["capabilities"], list) or any(
            type(c) is not str for c in binding["capabilities"]):
        raise ValueError("binding capabilities must be an array of strings")
    evidence = exact_object(binding["evidence"], "ref confidence eligibility")
    for key, value in evidence.items():
        text_field(value, "evidence " + key)
    conversion = exact_object(binding["conversion"],
                              "scale offset target_precision invalid_ranges")
    for key in ("scale", "offset", "target_precision"):
        text_field(conversion[key], "conversion " + key)
    ranges = conversion["invalid_ranges"]
    if not isinstance(ranges, list):
        raise ValueError("conversion invalid_ranges must be an array")
    for item in ranges:
        for key, value in exact_object(item, "minimum maximum").items():
            text_field(value, "invalid range " + key)
    text_field(binding["invalid_value"], "binding invalid_value")
    return Rule(Match(tuple(lowered), match["unit"], length, order, signed), binding)


def load_pack(text: str) -> Pack:
    """Read one reviewed version-1 recognition pack from strict JSON."""
    value = exact_object(strict_json(text),
                         "version id revision profile_id semantic_policy_revision rules")
    if type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("expected a version-1 recognition pack")
    if not isinstance(value["rules"], list):
        raise ValueError("rules must be an array")
    return Pack(text_field(value["id"], "id", identifier=True),
                text_field(value["revision"], "revision", identifier=True),
                text_field(value["profile_id"], "profile_id", identifier=True),
                text_field(value["semantic_policy_revision"], "semantic_policy_revision",
                           identifier=True),
                tuple(_rule(item, index) for index, item in enumerate(value["rules"])))


@dataclass(frozen=True)
class Candidate:
    rule: Rule
    message: Message
    signal: Signal


@dataclass(frozen=True)
class Recognition:
    bound: tuple[Candidate, ...]  # one per canonical key, in key order
    ambiguous: tuple[tuple[str, tuple[Candidate, ...]], ...]  # key, competing candidates


def matches(rule: Rule, signal: Signal) -> bool:
    match = rule.match
    return (signal.name.lower() in match.aliases and signal.unit == match.unit
            and (match.length is None or signal.length == match.length)
            and (match.byte_order is None or signal.byte_order == match.byte_order)
            and (match.signed is None or signal.signed == match.signed))


def recognize(pack: Pack, messages: tuple[Message, ...]) -> Recognition:
    """Propose bindings from executable messages; competing fields exclude the key."""
    candidates: dict[str, dict[str, list[Candidate]]] = {}
    for rule in pack.rules:
        for message in messages:
            if not message.executable:
                continue
            for signal in message.signals:
                if matches(rule, signal):
                    candidates.setdefault(rule.binding["key"], {}).setdefault(
                        signal.field_id, []).append(Candidate(rule, message, signal))
    bound, ambiguous = [], []
    for key, found in sorted(candidates.items()):
        # Candidates for one field deduplicate only when their binding templates
        # agree; rule order never selects a winner.
        agree = all(len({json.dumps(c.rule.binding, sort_keys=True) for c in group}) == 1
                    for group in found.values())
        if len(found) == 1 and agree:
            bound.append(next(iter(found.values()))[0])
        else:
            ambiguous.append((key, tuple(sorted((group[0] for group in found.values()),
                                                key=lambda c: c.signal.field_id))))
    return Recognition(tuple(bound), tuple(ambiguous))
