"""Thin DBC import orchestration: `mapping.json` to a validated, identified v1 profile.

Stages are mapping load, source acquisition and parse, binding resolution,
registry derivation, profile construction, then the unmodified `validate_profile`
and `identify_profile`. Rich detail lives in the separate source report, never in
the strict v1 Diagnostic (host tools only).
"""

from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import re

from .canonical import ProfileIdentity, identify_profile
from .dbc_compose import Alias, ComposeError, compose
from .dbc_lexer import LexerError, LexerLimits, SourceRequest, Span, load_sources
from .dbc_mapping import (IDENTITY, MappingError, conversion_of, decimal_text, load_mapping,
                          locate, mapping_revision, semantic_revision, span_location,
                          strict_json)
from .dbc_parser import Message, ParserLimits, Signal, parse_tokens
from .dbc_registry import (ELIGIBILITY_SOURCE, EVIDENCE_CONFIDENCE, FRESHNESS_BASIS,
                           FRESHNESS_DECLARATION, conflicts, conversion_groups, derive_registry, evidence_ref)
from .model import FORMAT, VERSION
from .validation import ProfileValidationError

CATALOG = Path(__file__).resolve().parents[1] / "controller_signal_catalog.json"
GENERATOR_REVISION = "dbc-import.v1"
LIMITS = {"max_sources": 16, "max_source_bytes": 16 * 1024 * 1024, "max_token_bytes": 4096,
          "max_records": 65536, "max_fields": 65536, "max_diagnostics": 65536}
STAGES = ("acquisition", "parse", "mapping", "validation", "result")
ACQUISITION_CODES = frozenset({"source_io", "source_decode", "source_digest_mismatch",
                               "source_provenance_missing"})
PLACEHOLDER = "0" * 64
MAPPING_ID = "mapping.json"
PRECISION = {"number": "float32", "boolean": "boolean", "enum": "uint16"}
SEVERITY = {"unmapped_field": "info", "unsupported_message": "warning",
            "all_features_disabled": "warning"}
V1_CATEGORY = {"unmapped_field": "unmapped", "unsupported_message": "unsupported",
               "all_features_disabled": "all_features_disabled"}


@dataclass(frozen=True)
class ImportResult:
    """A usable profile and identity, or neither after a fatal error; always a report."""
    profile: dict | None
    identity: ProfileIdentity | None
    report: dict


def _location(span: Span) -> dict:
    return {"source_id": span.source_id, "line": span.line, "column": span.column,
            "end_line": span.end_line, "end_column": span.end_column}


def _item(code: str, stage: str, affected: str, source_ids: list[str], spans, reason: str,
          severity: str | None = None, mapping_locations=()) -> dict:
    """A report item; `mapping_locations` are ready `mapping.json` location dicts."""
    return {"code": code, "severity": severity or SEVERITY[code], "stage": stage,
            "affected": affected, "source_ids": sorted(set(source_ids)),
            "locations": [_location(span) for span in spans] + list(mapping_locations),
            "reason": reason}


def _placer(text: str):
    """Return place(path): the `mapping.json` location of a JSON path as a one-item list.

    A path that cannot be placed falls back to its parent, then to no location.
    """
    spans = locate(text)

    def place(path: str) -> list[dict]:
        while path:
            if path in spans:
                return [dict(span_location(path, spans[path]), source_id=MAPPING_ID)]
            path = re.sub(r"(\.[^.\[\]]+|\[\d+\])$", "", path) if path != "$" else ""
        return []
    return place


def _sorted(items: list[dict]) -> list[dict]:
    return sorted(items, key=lambda item: (
        STAGES.index(item["stage"]), item["source_ids"],
        [(l["line"], l["column"], l["end_line"], l["end_column"]) for l in item["locations"]],
        item["code"], item["affected"]))


def _fatal_item(error: Exception, source_id: str, stage: str | None = None) -> dict:
    if isinstance(error, ComposeError):
        return _item(error.code, "parse", error.spans[0].source_id,
                     sorted({span.source_id for span in error.spans}), error.spans,
                     error.reason, "error")
    if isinstance(error, ProfileValidationError):
        return _item(error.category, "validation", error.path, list(error.sources) or [source_id],
                     [], error.reason, "error")
    stage = stage or ("acquisition" if error.code in ACQUISITION_CODES else "parse")
    spans = [error.span] if error.span is not None else []
    return _item(error.code, stage, source_id, [source_id], spans, error.reason, "error")


def _failure(report: dict, items: list[dict]) -> ImportResult:
    return ImportResult(None, None, dict(report, diagnostics=_sorted(items)))


def _field(signal: Signal) -> dict:
    return {"id": signal.field_id, "name": signal.name, "sources": list(signal.sources),
            "start": signal.start, "length": signal.length, "byte_order": signal.byte_order,
            "signed": signal.signed, "scale": decimal_text(signal.scale),
            "offset": decimal_text(signal.offset), "minimum": decimal_text(signal.minimum),
            "maximum": decimal_text(signal.maximum), "unit": signal.unit,
            "value_table": [{"raw": str(entry.raw), "label": entry.label}
                            for entry in signal.value_table]}


def _incompatibility(binding: dict, entry: dict, signal: Signal) -> tuple[str, str] | None:
    """The first type/unit/conversion/choice mismatch as (JSON member suffix, reason)."""
    kind, identity = entry["type"], conversion_of(binding) == IDENTITY
    if kind != "enum" and "choices" in binding:
        return ".choices", "choices are only allowed on enum targets"
    if kind == "enum" and "choices" not in binding:
        return "", "an enum target requires a choices map"
    if kind == "number":
        if signal.unit == binding["unit"] and not identity:
            return ".conversion", "a non-identity conversion is not allowed between equal units"
        if signal.unit != binding["unit"] and "conversion" not in binding:
            return "", "a conversion is required between different units"
        return None
    if binding["unit"]:
        return ".unit", "boolean and enum targets have an empty unit"
    if not identity:
        return ".conversion", "boolean and enum targets allow only an identity conversion"
    if signal.unit and "conversion" not in binding:
        return "", "a field with a unit needs an explicit identity conversion for this target"
    if kind == "boolean":
        if (signal.length, signal.signed, signal.minimum, signal.maximum) != (1, False, 0, 1):
            return "", "a boolean target needs an unsigned one-bit field with range 0 to 1"
        return None
    labels = {entry_.label for entry_ in signal.value_table}
    for label, key in sorted(binding["choices"].items()):
        if label not in labels:
            return ".choices", f"label {label!r} is not in the field's value table"
        if key not in entry["choices"]:
            return ".choices", f"choice key {key!r} is not a choice of the target"
    return None


def _resolve(mapping: dict, messages: tuple[Message, ...], aliases: tuple[Alias, ...],
             catalog: dict, source_ids: list[str],
             place) -> tuple[list[tuple[dict, Signal]], list[dict]]:
    """Resolve bindings to composed signals by any original name; return (resolved, fatal items).

    `place(path)` gives the `mapping.json` location list for a JSON path.
    """
    message_names: dict[str, set[str]] = {}
    for alias in aliases:
        if alias.kind == "message":
            message_names.setdefault(alias.representative, set()).add(alias.name)
    resolved, errors, targets, index_of = [], [], set(), {}

    def error(code: str, target: str, at: str, spans, reason: str) -> None:
        errors.append(_item(code, "mapping", target, source_ids, spans, reason, "error",
                            place(at)))

    for index, binding in enumerate(mapping["bindings"]):
        at, target = f"$.bindings[{index}]", binding["target"]
        if target in targets:
            error("duplicate_target", target, at + ".target", [],
                  "target is bound more than once")
            continue
        targets.add(target)
        entry = catalog["signals"].get(target)
        if entry is None:
            error("unknown_target", target, at + ".target", [],
                  "target is not in the controller signal catalog")
            continue
        found = [m for m in messages
                 if binding["message"] in message_names.get(m.message_id, {m.name})]
        if len(found) != 1:
            error("stale_mapping", target, at + ".message", [], "message name matches " + (
                "no message" if not found else "more than one message") + " in the source")
            continue
        message = found[0]
        if not message.executable:
            error("unsupported_required_binding", target, at, message.spans,
                  _unsupported_reason(message, "mapped message"))
            continue
        signals = [s for s in message.signals if binding["field"] in s.names]
        if len(signals) != 1:
            error("stale_mapping", target, at + ".field", message.spans,
                  "field is not in the mapped message")
            continue
        signal = signals[0]
        mismatch = _incompatibility(binding, entry, signal)
        if mismatch:
            error("incompatible_target", target, at + mismatch[0], signal.spans, mismatch[1])
        else:
            resolved.append((binding, signal))
            index_of[target] = index
    for (source, unit), members in conflicts(conversion_groups(
            (binding, signal.unit) for binding, signal in resolved)):
        errors.append(_item(
            "conflicting_conversion", "mapping", f"{source}->{unit}", source_ids, [],
            "bindings of one unit pair disagree on the conversion: " + ", ".join(members),
            "error", [loc for target in members for loc in place(f"$.bindings[{index_of[target]}]")]))
    return resolved, errors


def _binding(index: int, binding: dict, signal: Signal, mapping: dict, entry: dict) -> dict:
    scale, offset = conversion_of(binding)
    kind = entry["type"]
    conversion = {"source_unit": signal.unit, "target_unit": binding["unit"], "scale": scale,
                  "offset": offset, "target_precision": PRECISION[kind], "invalid_ranges": []}
    ref = evidence_ref(mapping["profile_id"])
    freshness = ({"timeout_ms": binding["timeout_ms"], "basis": FRESHNESS_BASIS,
                  "evidence_ref": ref} if "timeout_ms" in binding else None)
    enum_map = []
    if kind == "enum":
        enum_map = [{"raw": str(label.raw), "choice": binding["choices"][label.label]}
                    for label in sorted(signal.value_table, key=lambda label: label.raw)
                    if label.label in binding["choices"]]
    return {"key": binding["target"], "runtime_id": index, "type": kind,
            "unit": binding["unit"], "capabilities": list(entry["capabilities"]),
            "choices": [{"key": key, "value": value}
                        for value, key in enumerate(entry.get("choices", []))],
            "kind": "dbc", "inputs": [signal.field_id],
            "policy": {"conversion": conversion, "enum_map": enum_map,
                       "invalid_value": "unavailable"},
            "evidence": {"ref": ref, "confidence": EVIDENCE_CONFIDENCE,
                         "eligibility": ELIGIBILITY_SOURCE},
            "freshness": freshness}


def _alias_report(aliases: tuple[Alias, ...]) -> list[dict]:
    """Original-name and location inventory beside the one-representative v1 profile."""
    entries: dict[tuple[str, str], list[dict]] = {}
    for alias in aliases:
        entries.setdefault((alias.kind, alias.representative), []).append(
            {"source_id": alias.source_id, "name": alias.name, "id": alias.original_id,
             "locations": [_location(span) for span in alias.spans]})
    return [{"kind": kind, "id": rep, "aliases": sorted(
        found, key=lambda a: (a["source_id"], a["name"], a["id"]))}
        for (kind, rep), found in sorted(entries.items())]


def _unsupported_reason(message: Message, subject: str = "message") -> str:
    forms = ", ".join(message.unsupported_forms)
    return (f"{subject} is not standard classic CAN with length 0-8"
            + (f" (unsupported forms: {forms})" if forms else "")
            + ("" if subject != "message" else "; kept as inventory only"))


def _diagnostics(messages: tuple[Message, ...], bound: set[str],
                 source_ids: list[str]) -> list[dict]:
    items = []
    for message in messages:
        if not message.executable:
            items.append(_item("unsupported_message", "parse", message.message_id,
                               sorted({span.source_id for span in message.spans}),
                               message.spans, _unsupported_reason(message)))
            continue
        for signal in message.signals:
            if signal.field_id not in bound:
                items.append(_item("unmapped_field", "mapping", signal.field_id,
                                   list(signal.sources),
                                   signal.spans, "no mapping binds this field"))
    if not bound:
        items.append(_item("all_features_disabled", "result", "profile", source_ids, [],
                           "no bindings remain; startup stays black with no vehicle fallback"))
    return _sorted(items)


def _freshness_failure(text: str, error: MappingError, place) -> ImportResult:
    """Report a bad `timeout_ms` as a mapping diagnostic; the mapping never loaded.

    The loader rejected the file before revisions could be computed, so the report
    carries no revisions and no sources; shape was validated up to this binding.
    """
    document = strict_json(text)
    index = int(re.search(r"\[(\d+)\]", error.path)[1])
    source_ids = [PurePosixPath(source["path"]).stem for source in document["sources"]]
    report = {"profile_id": document["profile_id"], "semantic_revision": None,
              "mapping_revision": None, "sources": [],
              "freshness": FRESHNESS_DECLARATION}
    return _failure(report, [_item(
        "invalid_freshness", "mapping", document["bindings"][index]["target"], source_ids, [],
        str(error), "error", place(error.path))])


def _check_profile_directory(path: Path, mapping: dict, text: str) -> None:
    """Under `vehicle-profiles/<make>/<model>/`, `profile_id` must be `<make>.<model>`."""
    parts = path.resolve().parent.parts
    if len(parts) >= 3 and parts[-3] == "vehicle-profiles":
        expected = f"{parts[-2]}.{parts[-1]}"
        if mapping["profile_id"] != expected:
            error = MappingError(f"profile_id must be {expected} for this directory",
                                 path="$.profile_id")
            span = locate(text).get("$.profile_id")
            error.location = span_location("$.profile_id", span) if span else None
            raise error


def import_profile(mapping_path: Path | str) -> ImportResult:
    """Import the mapping's sources into a validated, identified profile.

    An unreadable or malformed mapping raises MappingError. An `invalid_freshness`
    binding, like the other per-binding mapping codes, and source, parse, binding and
    validation failures return a fatal report and no profile. Nothing is written.
    """
    path = Path(mapping_path)
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        raise MappingError("mapping file cannot be read") from None
    place = _placer(text)
    try:
        mapping = load_mapping(text)
    except MappingError as error:
        if error.code != "invalid_freshness":
            raise
        return _freshness_failure(text, error, place)
    _check_profile_directory(path, mapping, text)
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    requested = sorted(((PurePosixPath(source["path"]).stem, source)
                        for source in mapping["sources"]), key=lambda item: item[0])
    first_id = requested[0][0]
    semantic, whole = semantic_revision(mapping), mapping_revision(mapping)
    base_report = {"profile_id": mapping["profile_id"], "semantic_revision": semantic,
                   "mapping_revision": whole,
                   "sources": [], "freshness": FRESHNESS_DECLARATION}
    try:
        records = load_sources(
            [SourceRequest(source_id, str(path.parent / source["path"]), source["path"],
                           source["bus"], None, source["license"], source["encoding"],
                           source["sha256"])
             for source_id, source in requested],
            LexerLimits(LIMITS["max_sources"], LIMITS["max_source_bytes"],
                        LIMITS["max_token_bytes"]))
        inventories = [parse_tokens(record.tokens, record.source_id, record.bus,
                                    ParserLimits(LIMITS["max_records"], LIMITS["max_fields"]))
                       for record in records]
    except LexerError as error:
        return _failure(base_report, [_fatal_item(error, first_id)])
    report = dict(base_report, sources=[
        {"id": record.source_id, "logical_path": record.logical_path, "bus": record.bus,
         "sha256": record.sha256, "version": inventory.version}
        for record, inventory in zip(records, inventories)])
    # Only selected-bus sources bind; other buses stay inventory in the report.
    selected = [(record, inventory) for record, inventory in zip(records, inventories)
                if record.bus == mapping["selected_bus"]]
    selected_ids = [record.source_id for record, _ in selected]
    try:
        messages, aliases = compose([inventory for _, inventory in selected])
    except ComposeError as error:
        return _failure(report, [_fatal_item(error, first_id)])
    resolved, errors = _resolve(mapping, messages, aliases, catalog, selected_ids, place)
    if errors:
        return _failure(report, errors)
    items = _diagnostics(messages, {signal.field_id for _, signal in resolved}, selected_ids)
    if len(items) > LIMITS["max_diagnostics"]:
        error = LexerError("resource_overflow", "diagnostic count exceeds the import limit")
        return _failure(report, [_fatal_item(error, first_id, "result")])
    ordered = sorted(resolved, key=lambda pair: pair[0]["target"])
    document = {
        "format": FORMAT, "version": VERSION, "profile_id": mapping["profile_id"],
        "semantic_policy_revision": semantic, "recognition_rule_revision": whole,
        "selected_bus": mapping["selected_bus"],
        "sources": [{"id": record.source_id, "kind": "dbc", "path": record.logical_path,
                     "sha256": record.sha256, "revision": record.revision,
                     "license": record.license_ref, "bus": record.bus}
                    for record, _ in selected],
        "messages": [{"id": message.message_id, "bus": mapping["selected_bus"],
                      "identifier_format": message.identifier_format,
                      "identifier": message.wire_id, "dlc": message.length,
                      "fields": [_field(signal) for signal in message.signals]}
                     for message in messages if message.executable],
        "bindings": [_binding(index, binding, signal, mapping,
                              catalog["signals"][binding["target"]])
                     for index, (binding, signal) in enumerate(ordered, start=1)],
        "diagnostics": [{"category": V1_CATEGORY[item["code"]], "affected": item["affected"],
                         "sources": item["source_ids"], "reason": item["reason"]}
                        for item in items],
        "compatibility_identity": PLACEHOLDER, "artifact_identity": PLACEHOLDER}
    try:
        identity = identify_profile(document, policies=derive_registry(
                                        mapping, {b["target"]: sig.unit for b, sig in resolved}),
                                    generator_revision=GENERATOR_REVISION,
                                    validation_limits=LIMITS)
    except ProfileValidationError as error:
        return _failure(report, [_fatal_item(error, first_id)])
    return ImportResult(identity.profile, identity,
                        dict(report, diagnostics=items, aliases=_alias_report(aliases)))
