"""Thin DBC import orchestration: one source to a validated, identified v1 profile.

Stages are source acquisition and parse, minimal recognition, profile
construction, then the unmodified `validate_profile` and `identify_profile`.
Rich detail lives in the separate source report, never in the strict v1
Diagnostic (host tools only).
"""

from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

from .canonical import ProfileIdentity, identify_profile
from .dbc_lexer import LexerError, LexerLimits, SourceRequest, Span, load_sources
from .dbc_parser import ParserLimits, Signal, SourceInventory, parse_tokens
from .model import FORMAT, VERSION
from .recognition import (Candidate, Pack, Recognition, check_context, exact_object, recognize,
                          recognition_input, recognition_rule_revision, strict_json, text_field)
from .validation import ProfileValidationError, ValidationPolicies

LIMIT_KEYS = ("max_sources", "max_source_bytes", "max_token_bytes", "max_records",
              "max_fields", "max_diagnostics")
SOURCE_KEYS = "id path logical_path bus revision license_ref encoding"
STAGES = ("acquisition", "parse", "recognition", "validation", "result")
ACQUISITION_CODES = frozenset({"source_io", "source_decode", "source_digest_mismatch",
                               "source_provenance_missing"})
PLACEHOLDER = "0" * 64
SEVERITY = {"unmapped_field": "info", "ambiguous_binding": "warning",
            "unsupported_message": "warning", "all_features_disabled": "warning"}


@dataclass(frozen=True)
class ImportResult:
    """A usable profile and identity, or neither after a fatal error; always a report."""
    profile: dict | None
    identity: ProfileIdentity | None
    report: dict


def check_request(value: Any) -> dict:
    """Validate the exact request shape. Field-to-input assignments have no place in it."""
    request = exact_object(value, "version profile_id selected_bus pack context generator_revision "
                           "limits sources")
    if type(request["version"]) is not int or request["version"] != 1:
        raise ValueError("expected a version-1 import request")
    for key in ("profile_id", "selected_bus", "generator_revision"):
        text_field(request[key], key, identifier=True)
    pack = exact_object(request["pack"], "id revision")
    for key, item in pack.items():
        text_field(item, "pack " + key, identifier=True)
    check_context(request["context"])
    limits = exact_object(request["limits"], " ".join(LIMIT_KEYS))
    if any(type(item) is not int or item < 0 for item in limits.values()):
        raise ValueError("limits must be nonnegative integers")
    sources = request["sources"]
    if not isinstance(sources, list) or len(sources) != 1:
        raise ValueError("exactly one source is supported")
    for source in sources:
        exact_object(source, SOURCE_KEYS, "expected_sha256")
        for key in ("id", "bus"):
            text_field(source[key], "source " + key, identifier=True)
        for key in ("path", "logical_path", "encoding"):
            text_field(source[key], "source " + key)
        if source["revision"] is not None:
            text_field(source["revision"], "source revision")
        if type(source["license_ref"]) is not str:
            raise ValueError("source license_ref must be a string")
        if source["bus"] != request["selected_bus"]:
            raise ValueError("the source must be on the selected bus")
    return request


def load_request(text: str) -> dict:
    """Read an import request from strict JSON."""
    return check_request(strict_json(text))


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


def _fatal_item(error: Exception, source_id: str) -> dict:
    if isinstance(error, ProfileValidationError):
        return _item(error.category, "validation", error.path, list(error.sources) or [source_id],
                     [], error.reason, "error")
    stage = ("acquisition" if error.code in ACQUISITION_CODES else
             "result" if error.reason.startswith("diagnostic count") else "parse")
    spans = [error.span] if error.span is not None else []
    return _item(error.code, stage, source_id, [source_id], spans, error.reason, "error")


def _field(signal: Signal, source_id: str) -> dict:
    return {"id": signal.field_id, "name": signal.name, "sources": [source_id],
            "start": signal.start, "length": signal.length, "byte_order": signal.byte_order,
            "signed": signal.signed, "scale": decimal_text(signal.scale),
            "offset": decimal_text(signal.offset), "minimum": decimal_text(signal.minimum),
            "maximum": decimal_text(signal.maximum), "unit": signal.unit,
            "value_table": [{"raw": str(entry.raw), "label": entry.label}
                            for entry in signal.value_table]}


def _binding(candidate: Candidate) -> dict:
    template = candidate.rule.binding
    conversion = dict(template["conversion"], source_unit=candidate.signal.unit,
                      target_unit=template["unit"])
    return {"key": template["key"], "runtime_id": template["runtime_id"],
            "type": template["type"], "unit": template["unit"],
            "capabilities": list(template["capabilities"]), "choices": [], "kind": "dbc",
            "inputs": [candidate.signal.field_id],
            "policy": {"conversion": conversion, "enum_map": [],
                       "invalid_value": template["invalid_value"]},
            "evidence": dict(template["evidence"]), "freshness": None}


def _diagnostics(inventory: SourceInventory, recognition: Recognition) -> list[dict]:
    source_id = inventory.source_id
    items = []
    for message in inventory.messages:
        if not message.executable:
            items.append(_item("unsupported_message", "parse", message.message_id, [source_id],
                               message.spans, "message is not standard classic CAN with length "
                               "0-8; kept as inventory only"))
    bound = {candidate.signal.field_id for candidate in recognition.bound}
    contested = {c.signal.field_id for _, found in recognition.ambiguous for c in found}
    for message in inventory.messages:
        if message.executable:
            for signal in message.signals:
                if signal.field_id not in bound | contested:
                    items.append(_item("unmapped_field", "recognition", signal.field_id,
                                       [source_id], signal.spans,
                                       "no reviewed rule matches this field"))
    for key, found in recognition.ambiguous:
        items.append(_item("ambiguous_binding", "recognition", key, [source_id],
                           [span for c in found for span in c.signal.spans],
                           "competing fields match one canonical input; binding omitted"))
    if not recognition.bound:
        items.append(_item("all_features_disabled", "result", "profile", [source_id], [],
                           "no bindings remain; startup stays black with no vehicle fallback"))
    return _sorted(items)


def import_profile(request: dict, *, pack: Pack, policies: ValidationPolicies,
                   base_dir: Path | None = None) -> ImportResult:
    """Import the request's one source through `pack` into a validated, identified profile.

    Configuration mistakes raise ValueError. Source, parse and validation failures
    return a fatal report and no profile.
    """
    request = check_request(request)
    if (pack.pack_id, pack.revision) != (request["pack"]["id"], request["pack"]["revision"]):
        raise ValueError("pack does not match the requested pack id and revision")
    if pack.profile_id != request["profile_id"]:
        raise ValueError("pack is not scoped to the requested profile")
    (source,) = request["sources"]
    limits = request["limits"]
    preimage = recognition_input(pack.pack_id, pack.revision, request["context"])
    recognition_report = {"preimage": preimage, "revision": recognition_rule_revision(preimage)}
    root = Path.cwd() if base_dir is None else Path(base_dir)
    try:
        (record,) = load_sources(
            [SourceRequest(source["id"], str(root / source["path"]), source["logical_path"],
                           source["bus"], source["revision"], source["license_ref"],
                           source["encoding"], source.get("expected_sha256"))],
            LexerLimits(limits["max_sources"], limits["max_source_bytes"],
                        limits["max_token_bytes"]))
        inventory = parse_tokens(record.tokens, record.source_id, record.bus,
                                 ParserLimits(limits["max_records"], limits["max_fields"]))
    except LexerError as error:
        return ImportResult(None, None, {"sources": [], "recognition_input": recognition_report,
                                           "diagnostics": [_fatal_item(error, source["id"])]})
    report_source = {"id": record.source_id, "logical_path": record.logical_path,
                     "bus": record.bus, "sha256": record.sha256, "version": inventory.version}
    recognition = recognize(pack, inventory.messages, request["context"])
    items = _diagnostics(inventory, recognition)
    if len(items) > limits["max_diagnostics"]:
        error = LexerError("resource_overflow", "diagnostic count exceeds the request limit")
        return ImportResult(None, None, {"sources": [report_source],
                                           "recognition_input": recognition_report,
                                           "diagnostics": [_fatal_item(error, source["id"])]})
    v1_category = {"unmapped_field": "unmapped", "ambiguous_binding": "ambiguous",
                   "unsupported_message": "unsupported",
                   "all_features_disabled": "all_features_disabled"}
    document = {
        "format": FORMAT, "version": VERSION, "profile_id": request["profile_id"],
        "semantic_policy_revision": pack.semantic_policy_revision,
        "recognition_rule_revision": recognition_report["revision"], "selected_bus": request["selected_bus"],
        "sources": [{"id": record.source_id, "kind": "dbc", "path": record.logical_path,
                     "sha256": record.sha256, "revision": record.revision,
                     "license": record.license_ref, "bus": record.bus}],
        "messages": [{"id": message.message_id, "bus": record.bus,
                      "identifier_format": message.identifier_format,
                      "identifier": message.wire_id, "dlc": message.length,
                      "fields": [_field(signal, record.source_id) for signal in message.signals]}
                     for message in inventory.messages if message.executable],
        "bindings": [_binding(candidate) for candidate in recognition.bound],
        "diagnostics": [{"category": v1_category[item["code"]], "affected": item["affected"],
                         "sources": item["source_ids"], "reason": item["reason"]}
                        for item in items],
        "compatibility_identity": PLACEHOLDER, "artifact_identity": PLACEHOLDER}
    try:
        identity = identify_profile(document, policies=policies,
                                    generator_revision=request["generator_revision"],
                                    validation_limits=limits)
    except ProfileValidationError as error:
        return ImportResult(None, None, {"sources": [report_source],
                                           "recognition_input": recognition_report,
                                           "diagnostics": [_fatal_item(error, record.source_id)]})
    return ImportResult(identity.profile, identity,
                        {"sources": [report_source],
                                  "recognition_input": recognition_report, "diagnostics": items})
