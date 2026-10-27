"""Strict, lossless structural validation for normalized vehicle profiles."""

import json
import re
from copy import deepcopy
from dataclasses import dataclass
from fractions import Fraction
from pathlib import PurePosixPath
from typing import Any, cast

from .model import FORMAT, VERSION, Profile
from .bit_coordinates import occupied_bits

_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")
_DECIMAL = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\Z")


class ProfileValidationError(ValueError):
    """Stable category and JSON path, with applicable source identities."""

    def __init__(self, category: str, path: str, reason: str,
                 sources: tuple[str, ...] = ()) -> None:
        self.category = category
        self.path = path
        self.reason = reason
        self.sources = sources
        super().__init__(f"{category} at {path}: {reason}")


def _fail(category: str, path: str, reason: str) -> None:
    raise ProfileValidationError(category, path, reason)


def _object(value: Any, path: str, fields: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail("invalid_type", path, "expected object")
    if any(not isinstance(key, str) for key in value):
        _fail("invalid_type", path, "object keys must be strings")
    expected = set(fields.split())
    unknown = value.keys() - expected
    missing = expected - value.keys()
    if unknown:
        _fail("unknown_field", path, f"unknown fields: {', '.join(sorted(unknown))}")
    if missing:
        _fail("missing_field", path, f"missing fields: {', '.join(sorted(missing))}")
    return value


def _array(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        _fail("invalid_type", path, "expected array")
    return value


def _string(value: Any, path: str, *, empty: bool = False) -> str:
    if not isinstance(value, str):
        _fail("invalid_type", path, "expected string")
    if not empty and not value.strip():
        _fail("invalid_value", path, "expected nonblank string")
    return value


def _identifier(value: Any, path: str) -> str:
    value = _string(value, path)
    if not _ID.fullmatch(value):
        _fail("invalid_identity", path, "expected stable identifier")
    return value


def _int(value: Any, path: str) -> int:
    if type(value) is not int:
        _fail("invalid_type", path, "expected integer")
    return value


def _enum(value: Any, path: str, choices: tuple[str, ...]) -> str:
    value = _string(value, path)
    if value not in choices:
        _fail("invalid_value", path, f"expected one of {', '.join(choices)}")
    return value


def _lexeme(value: Any, path: str, pattern: re.Pattern[str]) -> None:
    if not pattern.fullmatch(_string(value, path)):
        _fail("invalid_numeric_lexeme", path, "invalid numeric string")


def _unique(value: str | int, seen: set[Any], path: str) -> None:
    if value in seen:
        _fail("duplicate_identity", path, f"duplicate identity {value!r}")
    seen.add(value)


def _refs(value: Any, path: str, known: set[str]) -> None:
    seen: set[Any] = set()
    for index, ref in enumerate(_array(value, path)):
        item = f"{path}[{index}]"
        _identifier(ref, item)
        _unique(ref, seen, item)
        if ref not in known:
            _fail("invalid_reference", item, f"unknown reference {ref!r}")


def _digest(value: Any, path: str) -> None:
    if not _DIGEST.fullmatch(_string(value, path)):
        _fail("invalid_identity", path, "expected lowercase SHA-256 hex digest")


def _binding(binding: Any, path: str, fields: set[str]) -> None:
    b = _object(binding, path, "key runtime_id type unit capabilities choices kind inputs policy evidence freshness")
    _identifier(b["key"], path + ".key")
    _int(b["runtime_id"], path + ".runtime_id")
    _enum(b["type"], path + ".type", ("boolean", "number", "enum"))
    _string(b["unit"], path + ".unit", empty=True)
    capabilities: set[Any] = set()
    for index, capability in enumerate(_array(b["capabilities"], path + ".capabilities")):
        p = f"{path}.capabilities[{index}]"
        _enum(capability, p, ("read", "notify"))
        _unique(capability, capabilities, p)
    choices: set[Any] = set()
    for index, choice in enumerate(_array(b["choices"], path + ".choices")):
        p = f"{path}.choices[{index}]"
        c = _object(choice, p, "key value")
        _identifier(c["key"], p + ".key")
        _int(c["value"], p + ".value")
        _unique(c["key"], choices, p + ".key")
    _refs(b["inputs"], path + ".inputs", fields)
    kind = _enum(b["kind"], path + ".kind", ("native", "dbc"))
    policy = _object(b["policy"], path + ".policy",
                     "handler revision invalid_value" if kind == "native" else
                     "conversion enum_map invalid_value")
    _string(policy["invalid_value"], path + ".policy.invalid_value")
    if kind == "native":
        _identifier(policy["handler"], path + ".policy.handler")
        _identifier(policy["revision"], path + ".policy.revision")
    else:
        conversion = _object(policy["conversion"], path + ".policy.conversion",
                             "source_unit target_unit scale offset target_precision invalid_ranges")
        for key in ("source_unit", "target_unit"):
            _string(conversion[key], path + ".policy.conversion." + key, empty=True)
        for key in ("scale", "offset"):
            _lexeme(conversion[key], path + ".policy.conversion." + key, _DECIMAL)
        _string(conversion["target_precision"], path + ".policy.conversion.target_precision")
        for index, invalid_range in enumerate(_array(conversion["invalid_ranges"], path + ".policy.conversion.invalid_ranges")):
            p = f"{path}.policy.conversion.invalid_ranges[{index}]"
            r = _object(invalid_range, p, "minimum maximum")
            for key in ("minimum", "maximum"):
                _lexeme(r[key], p + "." + key, _DECIMAL)
        for index, item in enumerate(_array(policy["enum_map"], path + ".policy.enum_map")):
            p = f"{path}.policy.enum_map[{index}]"
            m = _object(item, p, "raw choice")
            _lexeme(m["raw"], p + ".raw", _INTEGER)
            _identifier(m["choice"], p + ".choice")
            if m["choice"] not in choices:
                _fail("invalid_reference", p + ".choice", "unknown canonical choice")
    evidence = _object(b["evidence"], path + ".evidence", "ref confidence eligibility")
    for key, value in evidence.items():
        _string(value, path + ".evidence." + key)
    if b["freshness"] is not None:
        freshness = _object(b["freshness"], path + ".freshness", "timeout_ms basis evidence_ref")
        _int(freshness["timeout_ms"], path + ".freshness.timeout_ms")
        _string(freshness["basis"], path + ".freshness.basis")
        _string(freshness["evidence_ref"], path + ".freshness.evidence_ref")


def validate_structure(document: Any) -> Profile:
    """Validate exact v1 shapes and references without changing input lexemes."""
    root = _object(document, "$", "format version profile_id semantic_policy_revision recognition_rule_revision selected_bus sources messages bindings diagnostics compatibility_identity artifact_identity")
    if root["format"] != FORMAT:
        _fail("unsupported_format", "$.format", f"expected {FORMAT!r}")
    if _int(root["version"], "$.version") != VERSION:
        _fail("unsupported_version", "$.version", f"expected {VERSION}")
    for key in ("profile_id", "semantic_policy_revision", "recognition_rule_revision", "selected_bus"):
        _identifier(root[key], "$." + key)
    for key in ("compatibility_identity", "artifact_identity"):
        _digest(root[key], "$." + key)
    sources: set[str] = set()
    for index, source in enumerate(_array(root["sources"], "$.sources")):
        p = f"$.sources[{index}]"
        s = _object(source, p, "id kind path sha256 revision license bus")
        _unique(_identifier(s["id"], p + ".id"), sources, p + ".id")
        try:
            _enum(s["kind"], p + ".kind", ("dbc", "native"))
            logical_path = _string(s["path"], p + ".path")
            if PurePosixPath(logical_path).is_absolute() or ".." in PurePosixPath(logical_path).parts or "\\" in logical_path or re.match(r"^[A-Za-z]:", logical_path):
                _fail("invalid_source_path", p + ".path", "expected logical package-relative path")
            _digest(s["sha256"], p + ".sha256")
            if s["revision"] is not None:
                _string(s["revision"], p + ".revision")
            _string(s["license"], p + ".license")
            _identifier(s["bus"], p + ".bus")
        except ProfileValidationError as error:
            error.sources = (s["id"],)
            raise
    messages: set[Any] = set()
    fields: set[str] = set()
    for index, message in enumerate(_array(root["messages"], "$.messages")):
        p = f"$.messages[{index}]"
        m = _object(message, p, "id bus identifier_format identifier dlc fields")
        _unique(_identifier(m["id"], p + ".id"), messages, p + ".id")
        _identifier(m["bus"], p + ".bus")
        _enum(m["identifier_format"], p + ".identifier_format", ("standard", "extended"))
        _int(m["identifier"], p + ".identifier")
        _int(m["dlc"], p + ".dlc")
        for field_index, field in enumerate(_array(m["fields"], p + ".fields")):
            fp = f"{p}.fields[{field_index}]"
            f = _object(field, fp, "id name sources start length byte_order signed scale offset minimum maximum unit value_table")
            _unique(_identifier(f["id"], fp + ".id"), fields, fp + ".id")
            try:
                _string(f["name"], fp + ".name")
                _refs(f["sources"], fp + ".sources", sources)
                for key in ("start", "length"):
                    _int(f[key], fp + "." + key)
                _enum(f["byte_order"], fp + ".byte_order", ("intel", "motorola"))
                if type(f["signed"]) is not bool:
                    _fail("invalid_type", fp + ".signed", "expected boolean")
                for key in ("scale", "offset", "minimum", "maximum"):
                    _lexeme(f[key], fp + "." + key, _DECIMAL)
                _string(f["unit"], fp + ".unit", empty=True)
                for value_index, value in enumerate(_array(f["value_table"], fp + ".value_table")):
                    vp = f"{fp}.value_table[{value_index}]"
                    v = _object(value, vp, "raw label")
                    _lexeme(v["raw"], vp + ".raw", _INTEGER)
                    _string(v["label"], vp + ".label")
            except ProfileValidationError as error:
                if isinstance(f["sources"], list):
                    error.sources = tuple(ref for ref in f["sources"] if isinstance(ref, str))
                raise
    keys: set[Any] = set()
    runtime_ids: set[Any] = set()
    for index, binding in enumerate(_array(root["bindings"], "$.bindings")):
        p = f"$.bindings[{index}]"
        _binding(binding, p, fields)
        _unique(binding["key"], keys, p + ".key")
        _unique(binding["runtime_id"], runtime_ids, p + ".runtime_id")
    for index, diagnostic in enumerate(_array(root["diagnostics"], "$.diagnostics")):
        p = f"$.diagnostics[{index}]"
        d = _object(diagnostic, p, "category affected sources reason")
        _enum(d["category"], p + ".category", ("unmapped", "ambiguous", "unsupported", "all_features_disabled"))
        _identifier(d["affected"], p + ".affected")
        _refs(d["sources"], p + ".sources", sources)
        _string(d["reason"], p + ".reason")
    if not root["bindings"] and not any(
            d["category"] == "all_features_disabled" for d in root["diagnostics"]):
        _fail("missing_empty_profile_diagnostic", "$.diagnostics",
              "empty profile must explicitly report all_features_disabled")
    return cast(Profile, root)


def load_profile(text: str) -> Profile:
    """Parse JSON, rejecting duplicate keys before structural validation."""
    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in items:
            if key in result:
                _fail("duplicate_key", "$", f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    def invalid_constant(value: str) -> None:
        _fail("malformed_json", "$", f"non-finite JSON number {value}")

    try:
        return validate_structure(json.loads(text, object_pairs_hook=pairs,
                                             parse_constant=invalid_constant))
    except json.JSONDecodeError as error:
        raise ProfileValidationError("malformed_json", "$", error.msg) from error


@dataclass(frozen=True)
class HandlerPolicy:
    """A reviewed handler signature, explicitly scoped to profiles."""

    profiles: frozenset[str]
    value_type: str
    unit: str
    minimum_inputs: int
    maximum_inputs: int


@dataclass(frozen=True)
class ValidationPolicies:
    """Reviewed references supplied by the caller, never inherited from Mazda.

    Unit conversion coefficients describe target = source * scale + offset.
    Evidence entries are exact (reference, confidence, eligibility) triples.
    """

    profile_revisions: dict[str, tuple[str, str]]
    handlers: dict[tuple[str, str], HandlerPolicy]
    evidence: frozenset[tuple[str, str, str]]
    units: frozenset[str]
    unit_conversions: dict[tuple[str, str], tuple[str, str]]
    invalid_value_policies: frozenset[str]
    freshness_policies: dict[tuple[str, str], tuple[int, str, str]]


def _semantic_fail(category: str, path: str, reason: str,
                   sources: list[str] | tuple[str, ...] = ()) -> None:
    raise ProfileValidationError(category, path, reason, tuple(sorted(set(sources))))


def _number(text: str, path: str, sources: list[str] = ()) -> Fraction:
    # Bound the host representation before constructing very large integers.
    # This is an input validation limit, not an approximation of the decimal.
    exponent = re.search(r"[eE]([+-]?[0-9]+)$", text)
    if len(text) > 1024 or (exponent and (len(exponent[1].lstrip("+-")) > 4 or
                                        abs(int(exponent[1])) > 1024)):
        _semantic_fail("invalid_numeric_range", path, "decimal exceeds supported exact representation", sources)
    return Fraction(text)


def _raw_integer(text: str, path: str, sources: list[str]) -> int:
    if len(text.lstrip("-")) > 20:
        _semantic_fail("invalid_enum", path, "raw integer exceeds a 64-bit field", sources)
    return int(text)


def _field_signature(field: dict[str, Any]) -> tuple[Any, ...]:
    return (field["start"], field["length"], field["byte_order"], field["signed"],
            *(_number(field[key], "$") for key in ("scale", "offset", "minimum", "maximum")),
            field["unit"], tuple(sorted((int(v["raw"]), v["label"]) for v in field["value_table"])))


def validate_profile(document: Any, *, policies: ValidationPolicies) -> Profile:
    """Validate executable mechanics/policy and compose equivalent definitions.

    The input is never mutated. Equivalent source fragments are composed in a
    deterministic order and binding references use the surviving field identity.
    Ambiguous recognition is represented by diagnostics with no exposed binding.
    This function does not parse DBC syntax or recognize source fields.
    """
    profile = deepcopy(validate_structure(document))
    expected = policies.profile_revisions.get(profile["profile_id"])
    if expected != (profile["semantic_policy_revision"], profile["recognition_rule_revision"]):
        _semantic_fail("unknown_policy", "$", "unreviewed profile or policy/recognition revision pair")
    sources = {source["id"]: source for source in profile["sources"]}
    fields: dict[str, dict[str, Any]] = {}
    selected_fields: set[str] = set()
    groups: dict[tuple[str, str, int], list[dict[str, Any]]] = {}
    for mi, message in enumerate(profile["messages"]):
        path = f"$.messages[{mi}]"
        message_sources = sorted({s for f in message["fields"] for s in f["sources"]})
        selected = message["bus"] == profile["selected_bus"]
        maximum_id = 0x7FF if message["identifier_format"] == "standard" else 0x1FFFFFFF
        if not 0 <= message["identifier"] <= maximum_id or not 0 <= message["dlc"] <= 8:
            _semantic_fail("invalid_message", path, "identifier or classic CAN payload outside range", message_sources)
        if selected and message["identifier_format"] != "standard":
            _semantic_fail("unsupported_selected_traffic", path, "selected input supports standard classic CAN only", message_sources)
        groups.setdefault((message["bus"], message["identifier_format"], message["identifier"]), []).append(message)
        for fi, field in enumerate(message["fields"]):
            fp = f"{path}.fields[{fi}]"
            refs = field["sources"]
            if not refs or any(sources[s]["kind"] != "dbc" or sources[s]["bus"] != message["bus"] for s in refs):
                _semantic_fail("invalid_source_reference", fp + ".sources", "DBC fields require matching-bus DBC provenance", refs)
            try:
                bits = occupied_bits(field["start"], field["length"], field["byte_order"])
            except ValueError as error:
                _semantic_fail("invalid_bit_coordinates", fp, str(error), refs)
            if any(bit >= message["dlc"] * 8 for bit in bits):
                _semantic_fail("invalid_bit_coordinates", fp, "occupied bit exceeds message payload", refs)
            scale, offset, minimum, maximum = (_number(field[k], fp + "." + k, refs)
                                                for k in ("scale", "offset", "minimum", "maximum"))
            if scale == 0 or minimum > maximum:
                _semantic_fail("invalid_numeric_range", fp, "scale must be nonzero and minimum must not exceed maximum", refs)
            low = -(1 << (field["length"] - 1)) if field["signed"] else 0
            high = (1 << (field["length"] - (1 if field["signed"] else 0))) - 1
            physical = sorted((low * scale + offset, high * scale + offset))
            if minimum < physical[0] or maximum > physical[1]:
                _semantic_fail("invalid_numeric_range", fp, "declared bounds exceed raw field range", refs)
            raw_values: set[int] = set()
            for vi, value in enumerate(field["value_table"]):
                raw = _raw_integer(value["raw"], f"{fp}.value_table[{vi}].raw", refs)
                if raw in raw_values or not low <= raw <= high:
                    _semantic_fail("invalid_enum", f"{fp}.value_table[{vi}]", "duplicate or out-of-range raw enum", refs)
                raw_values.add(raw)
            fields[field["id"]] = field
            if selected:
                selected_fields.add(field["id"])

    aliases: dict[str, str] = {}
    composed = []
    for _, messages in sorted(groups.items()):
        messages.sort(key=lambda item: item["id"])
        all_sources = sorted({s for m in messages for f in m["fields"] for s in f["sources"]})
        if len({m["dlc"] for m in messages}) != 1:
            _semantic_fail("source_conflict", "$.messages", "same message has contradictory payload sizes", all_sources)
        result = deepcopy(messages[0])
        result["fields"] = []
        for field in sorted((f for m in messages for f in m["fields"]), key=lambda item: item["id"]):
            bits = set(occupied_bits(field["start"], field["length"], field["byte_order"]))
            duplicate = None
            for existing in result["fields"]:
                overlap = bits.intersection(occupied_bits(existing["start"], existing["length"], existing["byte_order"]))
                if overlap and _field_signature(field) != _field_signature(existing):
                    _semantic_fail("source_conflict", "$.messages", "contradictory or unsupported overlapping fields",
                                   field["sources"] + existing["sources"])
                if overlap:
                    duplicate = existing
            if duplicate is not None:
                aliases[field["id"]] = duplicate["id"]
                duplicate["sources"] = sorted(set(duplicate["sources"] + field["sources"]))
            else:
                result["fields"].append(deepcopy(field))
                aliases[field["id"]] = field["id"]
        composed.append(result)
    profile["messages"] = composed
    fields = {field["id"]: field for message in composed for field in message["fields"]}
    selected_fields = {aliases[field_id] for field_id in selected_fields}
    for bi, binding in enumerate(profile["bindings"]):
        path = f"$.bindings[{bi}]"
        normalized_inputs = [aliases[fid] for fid in binding["inputs"]]
        if len(normalized_inputs) != len(set(normalized_inputs)):
            _semantic_fail("invalid_binding", path + ".inputs", "handler inputs must refer to distinct logical fields",
                           [s for fid in normalized_inputs for s in fields[fid]["sources"]])
        # Native handler inputs are positional; alias normalization preserves
        # their declared order rather than sorting arguments by source identity.
        binding["inputs"] = normalized_inputs
        refs = sorted({s for fid in binding["inputs"] for s in fields[fid]["sources"]})
        if any(fid not in selected_fields for fid in binding["inputs"]):
            _semantic_fail("invalid_binding", path + ".inputs", "binding references traffic outside selected bus", refs)
        if not 1 <= binding["runtime_id"] <= 0xFFFF:
            _semantic_fail("invalid_binding", path + ".runtime_id", "provider id must fit nonzero uint16", refs)
        if "read" not in binding["capabilities"]:
            _semantic_fail("invalid_capability", path, "exposed inputs require read capability", refs)
        if binding["unit"] not in policies.units:
            _semantic_fail("unknown_policy", path + ".unit", "unreviewed canonical unit", refs)
        values = [c["value"] for c in binding["choices"]]
        if len(values) != len(set(values)) or any(not 0 <= value <= 0xFFFF for value in values):
            _semantic_fail("invalid_enum", path + ".choices", "provider enum values must be unique uint16", refs)
        if (binding["type"] == "enum") != bool(binding["choices"]):
            _semantic_fail("invalid_enum", path + ".choices", "only enum inputs require choices", refs)
        if binding["type"] != "number" and binding["unit"]:
            _semantic_fail("invalid_binding", path + ".unit", "boolean and enum inputs have no unit", refs)
        evidence = binding["evidence"]
        if (evidence["ref"], evidence["confidence"], evidence["eligibility"]) not in policies.evidence:
            _semantic_fail("unknown_policy", path + ".evidence", "unreviewed evidence declaration", refs)
        policy = binding["policy"]
        if policy["invalid_value"] not in policies.invalid_value_policies or policy["invalid_value"] != "unavailable":
            _semantic_fail("unknown_policy", path + ".policy.invalid_value", "invalid observations must become unavailable", refs)
        if binding["freshness"] is not None:
            freshness = binding["freshness"]
            reviewed = policies.freshness_policies.get((profile["profile_id"], binding["key"]))
            if not 0 < freshness["timeout_ms"] <= 0xFFFFFFFF or freshness["basis"] != "reviewed_operational" or not any(
                    ref == freshness["evidence_ref"] and eligibility == "operational" for ref, _, eligibility in policies.evidence) or reviewed != (
                        freshness["timeout_ms"], freshness["basis"], freshness["evidence_ref"]):
                _semantic_fail("invalid_freshness", path + ".freshness", "requires positive bounded timeout and operational timing evidence", refs)
        if binding["kind"] == "native":
            handler = policies.handlers.get((policy["handler"], policy["revision"]))
            if handler is None or profile["profile_id"] not in handler.profiles:
                _semantic_fail("unknown_policy", path + ".policy", "handler revision is not reviewed for this profile", refs)
            if (handler.value_type, handler.unit) != (binding["type"], binding["unit"]) or not handler.minimum_inputs <= len(binding["inputs"]) <= handler.maximum_inputs:
                _semantic_fail("invalid_binding", path, "handler signature mismatch", refs)
        else:
            if len(binding["inputs"]) != 1:
                _semantic_fail("invalid_binding", path + ".inputs", "DBC extraction requires one field", refs)
            field = fields[binding["inputs"][0]]
            conversion = policy["conversion"]
            pair = (conversion["source_unit"], conversion["target_unit"])
            coefficients = policies.unit_conversions.get(pair)
            scale = _number(conversion["scale"], path + ".policy.conversion.scale", refs)
            offset = _number(conversion["offset"], path + ".policy.conversion.offset", refs)
            if (scale == 0 or pair != (field["unit"], binding["unit"]) or coefficients is None or
                    (scale, offset) != tuple(_number(x, path) for x in coefficients)):
                _semantic_fail("invalid_conversion", path + ".policy.conversion", "conversion must match reviewed unit normalization", refs)
            expected_precision = {"number": "float32", "boolean": "boolean", "enum": "uint16"}[binding["type"]]
            if conversion["target_precision"] != expected_precision:
                _semantic_fail("invalid_conversion", path + ".policy.conversion.target_precision", "precision must match provider value type", refs)
            bounds = [_number(field[k], path, refs) for k in ("minimum", "maximum")]
            for invalid in conversion["invalid_ranges"]:
                low, high = (_number(invalid[k], path, refs) for k in ("minimum", "maximum"))
                if low > high or low < bounds[0] or high > bounds[1]:
                    _semantic_fail("invalid_conversion", path + ".policy.conversion.invalid_ranges", "invalid range must be ordered within source physical bounds", refs)
            converted = [value * scale + offset for value in bounds]
            float_max = Fraction((2 ** 24 - 1) * 2 ** 104)
            if binding["type"] == "number" and (any(abs(value) > float_max or 0 < abs(value) < Fraction(1, 2 ** 149) for value in converted) or abs(_number(field["scale"], path) * scale) < Fraction(1, 2 ** 149)):
                _semantic_fail("invalid_conversion", path, "conversion cannot fit finite float32 values", refs)
            if binding["type"] == "boolean" and (set(converted) != {Fraction(0), Fraction(1)} or field["length"] != 1):
                _semantic_fail("invalid_conversion", path, "boolean extraction requires an explicit one-bit zero/one field", refs)
            raw_map = [_raw_integer(mapping["raw"], path + ".policy.enum_map", refs) for mapping in policy["enum_map"]]
            raw_min = -(1 << (field["length"] - 1)) if field["signed"] else 0
            raw_max = (1 << (field["length"] - (1 if field["signed"] else 0))) - 1
            if len(set(raw_map)) != len(raw_map) or any(not raw_min <= raw <= raw_max for raw in raw_map):
                _semantic_fail("invalid_enum", path + ".policy.enum_map", "duplicate or out-of-range enum mapping", refs)
            if (binding["type"] == "enum") != bool(raw_map):
                _semantic_fail("invalid_enum", path + ".policy.enum_map", "only enum bindings require raw mappings", refs)
    exposed = {binding["key"] for binding in profile["bindings"]}
    used_fields = {fid for binding in profile["bindings"] for fid in binding["inputs"]}
    for di, diagnostic in enumerate(profile["diagnostics"]):
        if diagnostic["category"] in ("ambiguous", "unsupported") and (diagnostic["affected"] in exposed or aliases.get(diagnostic["affected"], diagnostic["affected"]) in used_fields):
            _semantic_fail("invalid_binding", f"$.diagnostics[{di}]", "ambiguous or unsupported input must not be exposed", diagnostic["sources"])
        diagnostic["affected"] = aliases.get(diagnostic["affected"], diagnostic["affected"])
    return profile
