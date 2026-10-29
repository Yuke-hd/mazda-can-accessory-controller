"""Strict `mapping.json` loader: the one per-profile input besides the DBC files.

A mapping is JSON data (duplicate keys and nonfinite numbers rejected), never
executable code. This minimal loader checks shape only; target, type and unit
compatibility are checked against the signal catalog at import time (host tools
only).
"""

from fractions import Fraction
import hashlib
import json
import re
from pathlib import PurePosixPath
from typing import Any

_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
# The source lexer's exact-decimal grammar (see dbc-import "Supported DBC subset").
_DECIMAL = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
ENCODINGS = ("utf-8", "cp1252")
TOP_KEYS = "version profile_id selected_bus sources bindings"
SOURCE_KEYS = "path sha256 bus license"
DEFAULT_ENCODING = "utf-8"
BINDING_KEYS = "target message field unit"
BINDING_OPTIONAL = "conversion timeout_ms"
MAX_TIMEOUT_MS = 0xFFFFFFFF
IDENTITY = ("1", "0")


class MappingError(ValueError):
    """A mapping that cannot be read or is not an exact version-1 mapping."""

    def __init__(self, reason: str, code: str = "invalid_mapping") -> None:
        super().__init__(reason)
        self.code = code


def decimal_text(value: Fraction) -> str:
    """Exact normalized-v1 decimal for a Fraction whose denominator is 2^a * 5^b."""
    twos = fives = 0
    denominator = value.denominator
    while denominator % 2 == 0:
        denominator, twos = denominator // 2, twos + 1
    while denominator % 5 == 0:
        denominator, fives = denominator // 5, fives + 1
    if denominator != 1:
        raise ValueError("value has no finite decimal form")
    places = max(twos, fives)
    scaled = abs(value.numerator) * 10 ** places // value.denominator
    digits = str(scaled).rjust(places + 1, "0")
    text = digits[:-places] + "." + digits[-places:] if places else digits
    return "-" + text if value < 0 and scaled else text


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


def _exact_decimal(value: Any, name: str) -> Fraction:
    """Parse a JSON string in the lexer's decimal grammar; JSON numbers are rejected."""
    if type(value) is not str or len(value) > 1024 or not _DECIMAL.match(value):
        raise MappingError(f"{name} must be an exact decimal string")
    exponent = re.search(r"[eE]([+-]?[0-9]+)$", value)
    if exponent and (len(exponent[1].lstrip("+-")) > 4 or abs(int(exponent[1])) > 1024):
        raise MappingError(f"{name} exponent is out of range")
    return Fraction(value)


def _binding(binding: Any) -> None:
    _object(binding, BINDING_KEYS, "binding", BINDING_OPTIONAL)
    for key in ("target", "message", "field"):
        _text(binding[key], "binding " + key)
    _text(binding["unit"], "binding unit", empty=True)
    if "conversion" in binding:
        conversion = _object(binding["conversion"], "scale offset", "conversion")
        scale = _exact_decimal(conversion["scale"], "conversion scale")
        offset = _exact_decimal(conversion["offset"], "conversion offset")
        if scale == 0:
            raise MappingError("conversion scale must not be zero", "incompatible_target")
        binding["conversion"] = {"scale": decimal_text(scale), "offset": decimal_text(offset)}
    if "timeout_ms" in binding:
        timeout = binding["timeout_ms"]
        if type(timeout) is not int or not 0 < timeout <= MAX_TIMEOUT_MS:
            raise MappingError("timeout_ms must be an integer in 1..4294967295",
                               "invalid_freshness")


def load_mapping(text: str) -> dict:
    """Read and shape-check one version-1 mapping; return the plain dict.

    Conversion decimals are rewritten to canonical decimal text; an explicit
    identity conversion stays present (it matters for different units).
    """
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
    for binding in _list(mapping["bindings"], "bindings"):
        _binding(binding)
    return mapping


def conversion_of(binding: dict) -> tuple[str, str]:
    """Canonical (scale, offset) texts; an omitted conversion is the identity."""
    conversion = binding.get("conversion")
    return (conversion["scale"], conversion["offset"]) if conversion else IDENTITY


def _digest(preimage: dict) -> str:
    data = json.dumps(preimage, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "mapping-v1:" + hashlib.sha256(data.encode("utf-8")).hexdigest()


def _binding_preimage(binding: dict) -> dict:
    """A binding without its identity conversion, so omission and identity hash alike."""
    result = {key: binding[key] for key in BINDING_KEYS.split()}
    if conversion_of(binding) != IDENTITY:
        result["conversion"] = dict(binding["conversion"])
    for key in ("timeout_ms", "choices"):
        if key in binding:
            result[key] = binding[key]
    return result


def _normalized(mapping: dict, *, sources: bool) -> dict:
    """Order-insensitive preimage; list order in the file never changes a digest."""
    bindings = [_binding_preimage(binding) for binding in mapping["bindings"]]
    result = {
        "version": mapping["version"], "profile_id": mapping["profile_id"],
        "selected_bus": mapping["selected_bus"],
        "bindings": sorted(bindings, key=lambda item: (
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
