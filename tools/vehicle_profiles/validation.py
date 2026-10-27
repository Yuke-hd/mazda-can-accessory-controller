"""Strict, lossless structural validation for normalized vehicle profiles."""

import json
import re
from pathlib import PurePosixPath
from typing import Any, cast

from .model import FORMAT, VERSION, Profile

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
