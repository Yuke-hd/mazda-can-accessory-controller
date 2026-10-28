"""Thin DBC import orchestration: `mapping.json` to a validated, identified v1 profile.

Stages are mapping load, source acquisition and parse, binding resolution,
registry derivation, profile construction, then the unmodified `validate_profile`
and `identify_profile`. Rich detail lives in the separate source report, never in
the strict v1 Diagnostic (host tools only).
"""

from dataclasses import dataclass
from fractions import Fraction
import json
from pathlib import Path, PurePosixPath

from .canonical import ProfileIdentity, identify_profile
from .dbc_lexer import LexerError, LexerLimits, SourceRequest, Span, load_sources
from .dbc_mapping import MappingError, load_mapping, mapping_revision, semantic_revision
from .dbc_parser import ParserLimits, Signal, SourceInventory, parse_tokens
from .dbc_registry import EVIDENCE_CONFIDENCE, EVIDENCE_ELIGIBILITY, derive_registry, evidence_ref
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


def _location(span: Span) -> dict:
    return {"source_id": span.source_id, "line": span.line, "column": span.column,
            "end_line": span.end_line, "end_column": span.end_column}


def _item(code: str, stage: str, affected: str, source_ids: list[str], spans, reason: str,
          severity: str | None = None) -> dict:
    return {"code": code, "severity": severity or SEVERITY[code], "stage": stage,
            "affected": affected, "source_ids": sorted(set(source_ids)),
            "locations": [_location(span) for span in spans], "reason": reason}


def _sorted(items: list[dict]) -> list[dict]:
    return sorted(items, key=lambda item: (
        STAGES.index(item["stage"]), item["source_ids"],
        [(l["line"], l["column"], l["end_line"], l["end_column"]) for l in item["locations"]],
        item["code"], item["affected"]))


def _fatal_item(error: Exception, source_id: str, stage: str | None = None) -> dict:
    if isinstance(error, ProfileValidationError):
        return _item(error.category, "validation", error.path, list(error.sources) or [source_id],
                     [], error.reason, "error")
    stage = stage or ("acquisition" if error.code in ACQUISITION_CODES else "parse")
    spans = [error.span] if error.span is not None else []
    return _item(error.code, stage, source_id, [source_id], spans, error.reason, "error")


def _failure(report: dict, items: list[dict]) -> ImportResult:
    return ImportResult(None, None, dict(report, diagnostics=_sorted(items)))


def _field(signal: Signal, source_id: str) -> dict:
    return {"id": signal.field_id, "name": signal.name, "sources": [source_id],
            "start": signal.start, "length": signal.length, "byte_order": signal.byte_order,
            "signed": signal.signed, "scale": decimal_text(signal.scale),
            "offset": decimal_text(signal.offset), "minimum": decimal_text(signal.minimum),
            "maximum": decimal_text(signal.maximum), "unit": signal.unit,
            "value_table": [{"raw": str(entry.raw), "label": entry.label}
                            for entry in signal.value_table]}


def _resolve(mapping: dict, inventory: SourceInventory, catalog: dict,
             source_id: str) -> tuple[list[tuple[dict, Signal]], list[dict]]:
    """Resolve bindings to signals; return (resolved, fatal items)."""
    conversions = {(s, t) for s, t, _, _ in mapping["unit_conversions"]}
    resolved, errors, targets = [], [], set()

    def error(code: str, target: str, spans, reason: str) -> None:
        errors.append(_item(code, "mapping", target, [source_id], spans, reason, "error"))

    for binding in mapping["bindings"]:
        target = binding["target"]
        if target in targets:
            error("duplicate_target", target, [], "target is bound more than once")
            continue
        targets.add(target)
        entry = catalog["signals"].get(target)
        if entry is None:
            error("unknown_target", target, [], "target is not in the controller signal catalog")
            continue
        found = [m for m in inventory.messages if m.name == binding["message"]]
        if len(found) != 1:
            error("stale_mapping", target, [], "message name matches " + (
                "no message" if not found else "more than one message") + " in the source")
            continue
        message = found[0]
        if not message.executable:
            error("unsupported_required_binding", target, message.spans,
                  "mapped message is not standard classic CAN with length 0-8")
            continue
        signals = [s for s in message.signals if s.name == binding["field"]]
        if len(signals) != 1:
            error("stale_mapping", target, message.spans, "field is not in the mapped message")
            continue
        signal = signals[0]
        if entry["type"] != "number":
            error("incompatible_target", target, signal.spans,
                  "only number targets are supported by this importer step")
        elif (signal.unit, binding["unit"]) not in conversions:
            error("incompatible_target", target, signal.spans,
                  "no declared unit conversion from the field unit to the binding unit")
        else:
            resolved.append((binding, signal))
    return resolved, errors


def _binding(index: int, binding: dict, signal: Signal, mapping: dict, entry: dict) -> dict:
    scale, offset = next((scale, offset) for s, t, scale, offset in mapping["unit_conversions"]
                         if (s, t) == (signal.unit, binding["unit"]))
    conversion = {"source_unit": signal.unit, "target_unit": binding["unit"], "scale": scale,
                  "offset": offset, "target_precision": "float32", "invalid_ranges": []}
    return {"key": binding["target"], "runtime_id": index, "type": "number",
            "unit": binding["unit"], "capabilities": list(entry["capabilities"]), "choices": [],
            "kind": "dbc", "inputs": [signal.field_id],
            "policy": {"conversion": conversion, "enum_map": [], "invalid_value": "unavailable"},
            "evidence": {"ref": evidence_ref(mapping["profile_id"]),
                         "confidence": EVIDENCE_CONFIDENCE, "eligibility": EVIDENCE_ELIGIBILITY},
            "freshness": None}


def _diagnostics(inventory: SourceInventory, bound: set[str], source_id: str) -> list[dict]:
    items = []
    for message in inventory.messages:
        if not message.executable:
            items.append(_item("unsupported_message", "parse", message.message_id, [source_id],
                               message.spans, "message is not standard classic CAN with length "
                               "0-8; kept as inventory only"))
            continue
        for signal in message.signals:
            if signal.field_id not in bound:
                items.append(_item("unmapped_field", "mapping", signal.field_id, [source_id],
                                   signal.spans, "no mapping binds this field"))
    if not bound:
        items.append(_item("all_features_disabled", "result", "profile", [source_id], [],
                           "no bindings remain; startup stays black with no vehicle fallback"))
    return _sorted(items)


def import_profile(mapping_path: Path | str) -> ImportResult:
    """Import the mapping's one source into a validated, identified profile.

    An unreadable or malformed mapping raises MappingError. Source, parse, binding
    and validation failures return a fatal report and no profile. Nothing is written.
    """
    path = Path(mapping_path)
    try:
        mapping = load_mapping(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError):
        raise MappingError("mapping file cannot be read") from None
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    (source,) = mapping["sources"]
    source_id = PurePosixPath(source["path"]).stem
    semantic, whole = semantic_revision(mapping), mapping_revision(mapping)
    base_report = {"profile_id": mapping["profile_id"], "semantic_revision": semantic,
                   "mapping_revision": whole,
                   "sources": []}
    try:
        (record,) = load_sources(
            [SourceRequest(source_id, str(path.parent / source["path"]), source["path"],
                           source["bus"], None, source["license"], source["encoding"],
                           source["sha256"])],
            LexerLimits(LIMITS["max_sources"], LIMITS["max_source_bytes"],
                        LIMITS["max_token_bytes"]))
        inventory = parse_tokens(record.tokens, record.source_id, record.bus,
                                 ParserLimits(LIMITS["max_records"], LIMITS["max_fields"]))
    except LexerError as error:
        return _failure(base_report, [_fatal_item(error, source_id)])
    report = dict(base_report, sources=[{
        "id": record.source_id, "logical_path": record.logical_path, "bus": record.bus,
        "sha256": record.sha256, "version": inventory.version}])
    resolved, errors = _resolve(mapping, inventory, catalog, source_id)
    if errors:
        return _failure(report, errors)
    items = _diagnostics(inventory, {signal.field_id for _, signal in resolved}, source_id)
    if len(items) > LIMITS["max_diagnostics"]:
        error = LexerError("resource_overflow", "diagnostic count exceeds the import limit")
        return _failure(report, [_fatal_item(error, source_id, "result")])
    ordered = sorted(resolved, key=lambda pair: pair[0]["target"])
    document = {
        "format": FORMAT, "version": VERSION, "profile_id": mapping["profile_id"],
        "semantic_policy_revision": semantic, "recognition_rule_revision": whole,
        "selected_bus": mapping["selected_bus"],
        "sources": [{"id": record.source_id, "kind": "dbc", "path": record.logical_path,
                     "sha256": record.sha256, "revision": record.revision,
                     "license": record.license_ref, "bus": record.bus}],
        "messages": [{"id": message.message_id, "bus": record.bus,
                      "identifier_format": message.identifier_format,
                      "identifier": message.wire_id, "dlc": message.length,
                      "fields": [_field(signal, record.source_id) for signal in message.signals]}
                     for message in inventory.messages if message.executable],
        "bindings": [_binding(index, binding, signal, mapping,
                              catalog["signals"][binding["target"]])
                     for index, (binding, signal) in enumerate(ordered, start=1)],
        "diagnostics": [{"category": V1_CATEGORY[item["code"]], "affected": item["affected"],
                         "sources": item["source_ids"], "reason": item["reason"]}
                        for item in items],
        "compatibility_identity": PLACEHOLDER, "artifact_identity": PLACEHOLDER}
    try:
        identity = identify_profile(document, policies=derive_registry(mapping),
                                    generator_revision=GENERATOR_REVISION,
                                    validation_limits=LIMITS)
    except ProfileValidationError as error:
        return _failure(report, [_fatal_item(error, record.source_id)])
    return ImportResult(identity.profile, identity, dict(report, diagnostics=items))
