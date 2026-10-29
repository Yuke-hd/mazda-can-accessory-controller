"""Strict `mapping.json` loader: the one per-profile input besides the DBC files.

A mapping is JSON data (duplicate keys and nonfinite numbers rejected), never
executable code. This loader checks shape only; target, type, unit, conversion and
choice compatibility are checked against the signal catalog at import time (host
tools only). Errors carry the offending JSON path and line/column where placeable.
"""

import bisect
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
BINDING_OPTIONAL = "conversion timeout_ms choices"
MAX_TIMEOUT_MS = 0xFFFFFFFF
IDENTITY = ("1", "0")

_BLANK = re.compile(r"[ \t\r\n]*")
_STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
_SCALAR = re.compile(r"[^\s,\]}]+")


class MappingError(ValueError):
    """A mapping that cannot be read or is not an exact version-1 mapping.

    `code` is `invalid_mapping` for file-shape errors; `invalid_freshness` is the one
    stable per-binding code the loader raises. `location` is the JSON path with a
    one-based line/column span ({"path", "line", "column", "end_line", "end_column"})
    when the offending value can be placed, else None.
    """

    def __init__(self, reason: str, code: str = "invalid_mapping",
                 path: str | None = None) -> None:
        super().__init__(reason)
        self.code = code
        self.path = path
        self.location: dict | None = None


def locate(text: str) -> dict[str, tuple[int, int, int, int]]:
    """Map JSON paths (`$.bindings[0].unit`) to (line, column, end_line, end_column).

    Positions are one-based and the end is exclusive. Returns {} for text it cannot
    scan; callers parse with `strict_json` first, so it only sees well-formed JSON.
    """
    starts = [0] + [match.end() for match in re.finditer("\n", text)]
    spans: dict[str, tuple[int, int, int, int]] = {}

    def position(offset: int) -> tuple[int, int]:
        line = bisect.bisect_right(starts, offset)
        return line, offset - starts[line - 1] + 1

    def skip(i: int) -> int:
        return _BLANK.match(text, i).end()

    def value(i: int, path: str) -> int:
        i = skip(i)
        begin, opener = i, text[i]
        if opener in "{[":
            closer = "}" if opener == "{" else "]"
            i = skip(i + 1)
            index = 0
            while text[i] != closer:
                if opener == "{":
                    key = _STRING.match(text, i)
                    i = skip(skip(key.end()) + 1)
                    i = value(i, f"{path}.{json.loads(key.group())}")
                else:
                    i = value(i, f"{path}[{index}]")
                    index += 1
                i = skip(i)
                if text[i] == ",":
                    i = skip(i + 1)
            i += 1
        elif opener == '"':
            i = _STRING.match(text, i).end()
        else:
            i = _SCALAR.match(text, i).end()
        spans[path] = position(begin) + position(i)
        return i
    try:
        value(0, "$")
    except (IndexError, AttributeError, ValueError, RecursionError):
        return {}
    return spans


def span_location(path: str, span: tuple[int, int, int, int]) -> dict:
    """A location dict for a `locate` span."""
    return dict(zip(("line", "column", "end_line", "end_column"), span), path=path)


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
        failure = MappingError(f"malformed JSON: {error.msg}", path="$")
        failure.location = {"path": "$", "line": error.lineno, "column": error.colno,
                            "end_line": error.lineno, "end_column": error.colno}
        raise failure from None


def _object(value: Any, keys: str, name: str, path: str, optional: str = "") -> dict:
    required, allowed = set(keys.split()), set(keys.split()) | set(optional.split())
    if not isinstance(value, dict) or not required <= set(value) <= allowed:
        raise MappingError(f"{name} must be an object with fields: {' '.join(sorted(allowed))}",
                           path=path)
    return value


def _text(value: Any, name: str, path: str, *, identifier: bool = False,
          empty: bool = False) -> str:
    if type(value) is not str or (not empty and not value) or (
            identifier and not _ID.match(value)):
        raise MappingError(f"{name} must be a{'n identifier' if identifier else ' string'}",
                           path=path)
    return value


def _list(value: Any, name: str, path: str) -> list:
    if not isinstance(value, list):
        raise MappingError(f"{name} must be an array", path=path)
    return value


def _source_path(value: Any, path: str) -> str:
    text = _text(value, "source path", path)
    parts = PurePosixPath(text).parts
    if (text.startswith("/") or "\\" in text or ".." in parts
            or re.match(r"^[A-Za-z]:", text)):
        raise MappingError("source path must stay inside the profile directory", path=path)
    return text


def _exact_decimal(value: Any, name: str, path: str) -> Fraction:
    """Parse a JSON string in the lexer's decimal grammar; JSON numbers are rejected."""
    if type(value) is not str or len(value) > 1024 or not _DECIMAL.match(value):
        raise MappingError(f"{name} must be an exact decimal string", path=path)
    exponent = re.search(r"[eE]([+-]?[0-9]+)$", value)
    if exponent and (len(exponent[1].lstrip("+-")) > 4 or abs(int(exponent[1])) > 1024):
        raise MappingError(f"{name} exponent is out of range", path=path)
    return Fraction(value)


def _binding(binding: Any, path: str) -> None:
    _object(binding, BINDING_KEYS, "binding", path, BINDING_OPTIONAL)
    for key in ("target", "message", "field"):
        _text(binding[key], "binding " + key, f"{path}.{key}")
    _text(binding["unit"], "binding unit", path + ".unit", empty=True)
    if "conversion" in binding:
        where = path + ".conversion"
        conversion = _object(binding["conversion"], "scale offset", "conversion", where)
        scale = _exact_decimal(conversion["scale"], "conversion scale", where + ".scale")
        offset = _exact_decimal(conversion["offset"], "conversion offset", where + ".offset")
        if scale == 0:
            # A malformed mapping file, not a per-binding code: plain invalid_mapping.
            raise MappingError("conversion scale must not be zero", path=where + ".scale")
        binding["conversion"] = {"scale": decimal_text(scale), "offset": decimal_text(offset)}
    if "choices" in binding:
        where = path + ".choices"
        choices = binding["choices"]
        if not isinstance(choices, dict) or not choices:
            raise MappingError("choices must be a non-empty object of label to choice key",
                               path=where)
        for label, choice in choices.items():
            if not label:
                raise MappingError("choices labels must not be empty", path=where)
            _text(choice, "choices choice key", f"{where}.{label}", identifier=True)
    if "timeout_ms" in binding:
        timeout = binding["timeout_ms"]
        if type(timeout) is not int or not 0 < timeout <= MAX_TIMEOUT_MS:
            raise MappingError("timeout_ms must be an integer in 1..4294967295",
                               "invalid_freshness", path + ".timeout_ms")


def _shape(document: Any) -> dict:
    mapping = _object(document, TOP_KEYS, "mapping", "$")
    if type(mapping["version"]) is not int or mapping["version"] != 1:
        raise MappingError("expected a version-1 mapping", path="$.version")
    _text(mapping["profile_id"], "profile_id", "$.profile_id", identifier=True)
    bus = _text(mapping["selected_bus"], "selected_bus", "$.selected_bus", identifier=True)
    sources = _list(mapping["sources"], "sources", "$.sources")
    if not sources:
        raise MappingError("at least one source is required", path="$.sources")
    for index, source in enumerate(sources):
        where = f"$.sources[{index}]"
        _object(source, SOURCE_KEYS, "source", where, "encoding")
        source.setdefault("encoding", DEFAULT_ENCODING)
        _source_path(source["path"], where + ".path")
        if type(source["sha256"]) is not str or not _SHA256.match(source["sha256"]):
            raise MappingError("source sha256 must be 64 lowercase hex digits",
                               path=where + ".sha256")
        _text(source["bus"], "source bus", where + ".bus", identifier=True)
        _text(source["license"], "source license", where + ".license", empty=True)
        if source["encoding"] not in ENCODINGS:
            raise MappingError("source encoding must be utf-8 or cp1252",
                               path=where + ".encoding")
    if not any(source["bus"] == bus for source in sources):
        raise MappingError("at least one source must be on the selected bus", path="$.sources")
    ids = [PurePosixPath(source["path"]).stem for source in sources]
    if len(set(ids)) != len(ids):
        raise MappingError("source file names must be unique; they supply the source ids",
                           path="$.sources")
    for index, binding in enumerate(_list(mapping["bindings"], "bindings", "$.bindings")):
        _binding(binding, f"$.bindings[{index}]")
    return mapping


def load_mapping(text: str) -> dict:
    """Read and shape-check one version-1 mapping; return the plain dict.

    Conversion decimals are rewritten to canonical decimal text. An explicit identity
    conversion stays in the returned dict as written (canonicalized): it is required
    between different units and accepted between equal ones. Only the revision
    preimages treat it like an omitted conversion. A `MappingError` carries
    `.location` (JSON path and line/column) when the offending value can be placed.
    """
    document = strict_json(text)
    try:
        return _shape(document)
    except MappingError as error:
        span = locate(text).get(error.path) if error.path else None
        if span:
            error.location = span_location(error.path, span)
        raise


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
