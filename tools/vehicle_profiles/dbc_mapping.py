"""Strict `mapping.json` loader: the one per-profile input besides the DBC files.

A mapping is JSON data (duplicate keys and nonfinite numbers rejected), never
executable code. This minimal loader checks shape only; target, type and unit
compatibility are checked against the signal catalog at import time (host tools
only).
"""

import hashlib
import json
import re
from pathlib import PurePosixPath
from typing import Any

_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_DECIMAL = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\Z")
ENCODINGS = ("utf-8", "cp1252")
TOP_KEYS = ("version profile_id selected_bus sources units unit_conversions "
            "freshness_policies bindings")
SOURCE_KEYS = "path sha256 bus license"
DEFAULT_ENCODING = "utf-8"
BINDING_KEYS = "target message field unit"


class MappingError(ValueError):
    """A mapping that cannot be read or is not an exact version-1 mapping."""


def strict_json(text: str) -> Any:
    """Parse JSON, rejecting duplicate object keys and nonfinite numbers."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise MappingError(f"duplicate key {key}")
            result[key] = value
        return result

    def invalid(value):
        raise MappingError(f"non-finite number {value}")
    try:
        return json.loads(text, object_pairs_hook=pairs, parse_constant=invalid)
    except json.JSONDecodeError as error:
        raise MappingError(f"malformed JSON: {error.msg}") from None


def _object(value: Any, keys: str, name: str, optional: str = "") -> dict:
    required, allowed = set(keys.split()), set(keys.split()) | set(optional.split())
    if not isinstance(value, dict) or not required <= set(value) <= allowed:
        raise MappingError(f"{name} must be an object with fields: {' '.join(sorted(allowed))}")
    return value


def _text(value: Any, name: str, *, identifier: bool = False, empty: bool = False) -> str:
    if type(value) is not str or (not empty and not value) or (
            identifier and not _ID.match(value)):
        raise MappingError(f"{name} must be a{'n identifier' if identifier else ' string'}")
    return value


def _list(value: Any, name: str) -> list:
    if not isinstance(value, list):
        raise MappingError(f"{name} must be an array")
    return value


def _source_path(value: Any) -> str:
    path = _text(value, "source path")
    parts = PurePosixPath(path).parts
    if (path.startswith("/") or "\\" in path or ".." in parts
            or re.match(r"^[A-Za-z]:", path)):
        raise MappingError("source path must stay inside the profile directory")
    return path


def load_mapping(text: str) -> dict:
    """Read and shape-check one version-1 mapping; return the plain dict."""
    mapping = _object(strict_json(text), TOP_KEYS, "mapping")
    if type(mapping["version"]) is not int or mapping["version"] != 1:
        raise MappingError("expected a version-1 mapping")
    _text(mapping["profile_id"], "profile_id", identifier=True)
    bus = _text(mapping["selected_bus"], "selected_bus", identifier=True)
    sources = _list(mapping["sources"], "sources")
    if len(sources) != 1:
        raise MappingError("exactly one source is supported")
    for source in sources:
        _object(source, SOURCE_KEYS, "source", "encoding")
        source.setdefault("encoding", DEFAULT_ENCODING)
        _source_path(source["path"])
        if type(source["sha256"]) is not str or not _SHA256.match(source["sha256"]):
            raise MappingError("source sha256 must be 64 lowercase hex digits")
        if source["bus"] != bus:
            raise MappingError("the source must be on the selected bus")
        _text(source["license"], "source license", empty=True)
        if source["encoding"] not in ENCODINGS:
            raise MappingError("source encoding must be utf-8 or cp1252")
    units = [_text(unit, "unit", empty=True) for unit in _list(mapping["units"], "units")]
    if len(set(units)) != len(units):
        raise MappingError("units must be unique")
    seen = set()
    for row in _list(mapping["unit_conversions"], "unit_conversions"):
        if not isinstance(row, list) or len(row) != 4 or any(type(v) is not str for v in row):
            raise MappingError("unit conversions are [source, target, scale, offset] strings")
        if not _DECIMAL.match(row[2]) or not _DECIMAL.match(row[3]):
            raise MappingError("unit conversion scale and offset must be decimal strings")
        if tuple(row[:2]) in seen:
            raise MappingError("duplicate unit conversion")
        seen.add(tuple(row[:2]))
    if _list(mapping["freshness_policies"], "freshness_policies"):
        raise MappingError("freshness policies are not supported yet")
    for binding in _list(mapping["bindings"], "bindings"):
        _object(binding, BINDING_KEYS, "binding")
        for key in ("target", "message", "field"):
            _text(binding[key], "binding " + key)
        _text(binding["unit"], "binding unit", empty=True)
    return mapping


def _digest(preimage: dict) -> str:
    data = json.dumps(preimage, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "mapping-v1:" + hashlib.sha256(data.encode("utf-8")).hexdigest()


def _normalized(mapping: dict, *, sources: bool) -> dict:
    """Order-insensitive preimage; list order in the file never changes a digest."""
    def by_json(items):
        return sorted(items, key=lambda item: json.dumps(item, sort_keys=True))
    result = {
        "version": mapping["version"], "profile_id": mapping["profile_id"],
        "selected_bus": mapping["selected_bus"], "units": sorted(mapping["units"]),
        "unit_conversions": sorted(tuple(row) for row in mapping["unit_conversions"]),
        "freshness_policies": by_json(mapping["freshness_policies"]),
        "bindings": sorted(mapping["bindings"], key=lambda item: (
            item["target"], json.dumps(item, sort_keys=True)))}
    if sources:
        result["sources"] = sorted(mapping["sources"], key=lambda item: item["path"])
    return result


def semantic_revision(mapping: dict) -> str:
    """`mapping-v1:` digest of the mapping without `sources`; compatibility-relevant."""
    return _digest(_normalized(mapping, sources=False))


def mapping_revision(mapping: dict) -> str:
    """`mapping-v1:` digest of the whole normalized mapping, sources included."""
    return _digest(_normalized(mapping, sources=True))
