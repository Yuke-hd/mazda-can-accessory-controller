"""Pure consumer projections of one validated, identified vehicle profile.

Publication and build selection belong to the artifact-set layer. None of these
outputs implements extraction, recognition, native handlers, or freshness.
"""

from dataclasses import dataclass
import json
from types import MappingProxyType
from typing import Any

from .canonical import identify_profile
from .validation import ProfileValidationError, ValidationPolicies

# Existing SignalCatalogExport v1 bounds, checked against the real C++ exporter
# by vehicle_profile_projection_test.py. Changing a bound requires protocol review.
CATALOG_LIMITS = MappingProxyType({"signals": 64, "choices": 32, "key_bytes": 63,
                  "token_bytes": 31, "document_bytes": 16384})
_UNITS = {"": ("None", "none"), "km/h": ("KilometresPerHour", "km/h"),
          "rpm": ("RevolutionsPerMinute", "rpm"),
          "m/s^2": ("MetresPerSecondSquared", "m/s^2")}
_TYPES = {"boolean": "Boolean", "number": "Number", "enum": "Enum"}
_CONFIDENCE = {"Reference": "reference", "Observed": "observed", "Confirmed": "confirmed",
               "user_mapped": "reference"}
# Imported user-mapped evidence is the lowest existing ValidationStatus.
_STATUS = {"user_mapped": "Reference"}


def _json(document: Any) -> bytes:
    return (json.dumps(document, ensure_ascii=True, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def _fail(path: str, reason: str) -> None:
    raise ProfileValidationError("projection_limit", path, reason)


def _text(value: str, path: str, maximum: int) -> int:
    try:
        size = len(value.encode("utf-8", errors="strict"))
    except UnicodeError:
        _fail(path, "text must be valid UTF-8")
    if not size or size > maximum:
        _fail(path, f"text must contain 1..{maximum} UTF-8 bytes")
    return size


def _string_view(text: str) -> str:
    # Fixed-width octal byte escapes avoid quote/newline injection, execution,
    # source-encoding dependence, and C++ hexadecimal escape continuation.
    data = text.encode("utf-8", errors="strict")
    escaped = "".join(f"\\{byte:03o}" for byte in data)
    return f'std::string_view{{"{escaped}", {len(data)}}}'


def _compiler_signals(bindings: list[dict[str, Any]]) -> dict[str, Any]:
    signals = {}
    for binding in bindings:
        item = {"type": binding["type"], "capabilities": sorted(binding["capabilities"])}
        if binding["type"] == "enum":
            item["choices"] = [choice["key"] for choice in sorted(binding["choices"], key=lambda c: c["key"])]
        signals[binding["key"]] = item
    return signals


@dataclass(frozen=True)
class ProfileProjections:
    """Complete in-memory results; consumers publish all three together."""

    compiler_catalog: bytes
    metadata_header: bytes
    capability_report: bytes


def project_profile(document: Any, *, policies: ValidationPolicies,
                    generator_revision: str,
                    validation_limits: Any = CATALOG_LIMITS) -> ProfileProjections:
    """Validate, identify, and project without mutating inputs or writing files."""
    profile = identify_profile(document, policies=policies,
                               generator_revision=generator_revision,
                               validation_limits=validation_limits).profile
    bindings = sorted(profile["bindings"], key=lambda b: b["key"])
    if len(bindings) > CATALOG_LIMITS["signals"]:
        _fail("$.bindings", "signal count exceeds existing catalog capacity")
    document_size = 3
    for index, binding in enumerate(bindings):
        path = f"$.bindings[{index}]"
        key_size = _text(binding["key"], path + ".key", CATALOG_LIMITS["key_bytes"])
        if binding["unit"] not in _UNITS:
            _fail(path + ".unit", "unit has no existing SignalUnit representation")
        confidence = binding["evidence"]["confidence"]
        if confidence not in _CONFIDENCE:
            _fail(path + ".evidence.confidence", "confidence has no existing ValidationStatus representation")
        unit_size = _text(_UNITS[binding["unit"]][1], path + ".unit", CATALOG_LIMITS["token_bytes"])
        evidence_size = _text(_CONFIDENCE[confidence], path + ".evidence.confidence", CATALOG_LIMITS["token_bytes"])
        size = 2 + 2 + (3 + key_size) + 4 + (3 + unit_size) + (3 + evidence_size)
        if binding["type"] == "enum":
            if len(binding["choices"]) > CATALOG_LIMITS["choices"]:
                _fail(path + ".choices", "choice count exceeds existing catalog capacity")
            size += 3 + 2
            for choice in binding["choices"]:
                size += 3 + _text(choice["key"], path + ".choices.key", CATALOG_LIMITS["key_bytes"])
        document_size += size
    if document_size > CATALOG_LIMITS["document_bytes"]:
        _fail("$.bindings", "encoded catalog exceeds existing document capacity")

    lines = ["// Generated metadata only: extraction and policy stay in the provider.",
             "#pragma once", "", "#include <array>", "#include <string_view>",
             '#include "vehicle_signals/signal_catalog.hpp"', "", "namespace vehicle_profile {", ""]
    for index, binding in enumerate(bindings):
        if binding["type"] == "enum":
            lines.append(f"inline constexpr vehicle_signals::SignalEnumChoice kChoices{index}[] = {{")
            for choice in sorted(binding["choices"], key=lambda c: c["key"]):
                lines.append(f"  {{{choice['value']}, {_string_view(choice['key'])}}},")
            lines.append("};")
    lines.append(f"inline constexpr std::array<vehicle_signals::SignalMetadata, {len(bindings)}> kSignals{{{{")
    for index, binding in enumerate(bindings):
        bits = (1 if "read" in binding["capabilities"] else 0) | (2 if "notify" in binding["capabilities"] else 0)
        choices = f"kChoices{index}, {len(binding['choices'])}" if binding["type"] == "enum" else "nullptr, 0"
        lines.append("  {vehicle_signals::SignalId{" + str(binding["runtime_id"]) + "}, " +
                     _string_view(binding["key"]) + ", vehicle_signals::SignalType::" + _TYPES[binding["type"]] +
                     ", vehicle_signals::SignalUnit::" + _UNITS[binding["unit"]][0] +
                     ", vehicle_signals::ValidationStatus::" +
                     _STATUS.get(binding["evidence"]["confidence"], binding["evidence"]["confidence"]) +
                     f", vehicle_signals::SignalCapabilities::from_bits({bits}), {choices}}},")
    lines += ["}};", "inline constexpr vehicle_signals::SignalCatalogView kCatalog{kSignals.data(), kSignals.size()};",
              'static_assert(kCatalog.well_formed(), "generated signal catalog must be well formed");',
              "", "} // namespace vehicle_profile", ""]
    report = {"version": 1, "profile_id": profile["profile_id"],
              "compatibility_identity": profile["compatibility_identity"],
              "artifact_identity": profile["artifact_identity"],
              "all_features_disabled": not bool(bindings),
              "catalog_document_bytes": document_size,
              "signals": [{"key": b["key"], "binding_kind": b["kind"],
                           "capabilities": sorted(b["capabilities"])} for b in bindings],
              "diagnostics": sorted(({**d, "sources": sorted(d["sources"])}
                                     for d in profile["diagnostics"]), key=_json)}
    return ProfileProjections(_json({"version": 1, "signals": _compiler_signals(bindings)}),
                              "\n".join(lines).encode("utf-8"), _json(report))


def catalog_only_compiler_json(signals: dict[str, Any]) -> bytes:
    """Wrap the trusted compiled Mazda export without inventing profile metadata.

    This deliberately emits no executable profile, C++ metadata, identities,
    extraction coordinates, freshness policy, or capability interpretation.
    """
    return _json({"version": 1, "signals": signals})
