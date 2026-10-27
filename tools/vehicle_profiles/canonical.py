"""Versioned, exact profile identity preimages (host tools only)."""

from copy import deepcopy
from dataclasses import dataclass
from decimal import Decimal
import hashlib
import json
import re
from typing import Any
from collections.abc import Mapping

from .model import Profile
from .validation import ValidationPolicies, validate_profile


def _decimal(text: str) -> str:
    """Encode a validated finite decimal without context rounding or exponents."""
    if not isinstance(text, str) or not re.fullmatch(
            r"-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?", text):
        raise ValueError("identity decimal must be a finite decimal string")
    suffix = re.search(r"[eE]([+-]?[0-9]+)$", text)
    if len(text) > 1024 or (suffix and (len(suffix[1].lstrip("+-")) > 4 or
                                       abs(int(suffix[1])) > 1024)):
        raise ValueError("identity decimal exceeds supported exact representation")
    sign, digits, exponent = Decimal(text).as_tuple()
    coefficient = "".join(str(digit) for digit in digits)
    if not any(digits):
        return "0"
    if exponent >= 0:
        value = coefficient + "0" * exponent
    else:
        point = len(coefficient) + exponent
        value = (coefficient[:point] + "." + coefficient[point:] if point > 0
                 else "0." + "0" * -point + coefficient)
        value = value.rstrip("0").rstrip(".")
    return ("-" if sign else "") + value


def _bytes(document: Any) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def _field(field: dict[str, Any]) -> dict[str, Any]:
    result = deepcopy(field)
    for key in ("scale", "offset", "minimum", "maximum"):
        result[key] = _decimal(result[key])
    result["sources"] = sorted(result["sources"])
    result["value_table"] = sorted(
        ({"raw": str(int(v["raw"])), "label": v["label"]} for v in result["value_table"]),
        key=lambda value: int(value["raw"]))
    return result


def _binding(binding: dict[str, Any]) -> dict[str, Any]:
    result = deepcopy(binding)
    result["capabilities"] = sorted(result["capabilities"])
    result["choices"] = sorted(result["choices"], key=lambda choice: choice["key"])
    if result["kind"] == "dbc":
        conversion = result["policy"]["conversion"]
        for key in ("scale", "offset"):
            conversion[key] = _decimal(conversion[key])
        conversion["invalid_ranges"] = sorted(
            ({key: _decimal(value) for key, value in interval.items()}
             for interval in conversion["invalid_ranges"]), key=_bytes)
        result["policy"]["enum_map"] = sorted(
            ({"raw": str(int(v["raw"])), "choice": v["choice"]}
             for v in result["policy"]["enum_map"]), key=lambda value: int(value["raw"]))
    return result


def _normalized(profile: Profile) -> dict[str, Any]:
    result = deepcopy(profile)
    del result["compatibility_identity"], result["artifact_identity"]
    result["sources"] = sorted(result["sources"], key=lambda source: source["id"])
    for message in result["messages"]:
        message["fields"] = sorted((_field(field) for field in message["fields"]),
                                   key=lambda field: field["id"])
    result["messages"].sort(key=lambda message: message["id"])
    result["bindings"] = sorted((_binding(binding) for binding in result["bindings"]),
                                key=lambda binding: binding["key"])
    for diagnostic in result["diagnostics"]:
        diagnostic["sources"] = sorted(diagnostic["sources"])
    result["diagnostics"].sort(key=_bytes)
    return result


def _validation(policies: ValidationPolicies, limits: Mapping[str, int]) -> dict[str, Any]:
    if not isinstance(limits, Mapping) or any(
            not isinstance(key, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.:-]*", key)
            or type(value) is not int or value < 0 for key, value in limits.items()):
        raise ValueError("validation_limits must map stable identifiers to nonnegative integers")
    return {
        "limits": dict(limits),
        "profile_revisions": sorted([key, *value] for key, value in policies.profile_revisions.items()),
        "handlers": sorted([name, revision, sorted(handler.profiles), handler.value_type,
                            handler.unit, handler.minimum_inputs, handler.maximum_inputs]
                           for (name, revision), handler in policies.handlers.items()),
        "evidence": sorted(list(value) for value in policies.evidence),
        "units": sorted(policies.units),
        "unit_conversions": sorted([source, target, _decimal(scale), _decimal(offset)]
                                   for (source, target), (scale, offset) in policies.unit_conversions.items()),
        "invalid_value_policies": sorted(policies.invalid_value_policies),
        "freshness_policies": sorted([profile, key, *value]
                                     for (profile, key), value in policies.freshness_policies.items()),
    }


@dataclass(frozen=True)
class ProfileIdentity:
    profile: Profile
    compatibility_preimage: bytes
    artifact_preimage: bytes


def identify_profile(document: Any, *, policies: ValidationPolicies,
                     generator_revision: str,
                     validation_limits: Mapping[str, int]) -> ProfileIdentity:
    """Validate, compose and return a copy with both computed SHA-256 digests.

    Caller-supplied digest placeholders are excluded from both preimages.
    Native input order remains significant, including after alias composition.
    """
    if not isinstance(generator_revision, str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9_.:-]*", generator_revision):
        raise ValueError("generator_revision must be an explicit stable identifier")
    profile = validate_profile(document, policies=policies)
    normalized = _normalized(profile)
    fields = {field["id"]: (message, field)
              for message in normalized["messages"] for field in message["fields"]}
    bindings = deepcopy(normalized["bindings"])
    for binding in bindings:
        # Runtime assignments are local projection details, never persisted
        # semantic identifiers. The full artifact retains them.
        del binding["runtime_id"]
        inputs = []
        for field_id in binding["inputs"]:
            message, field = fields[field_id]
            inputs.append({"message": {key: message[key] for key in
                                       ("bus", "identifier_format", "identifier", "dlc")},
                           "field": {key: value for key, value in field.items()
                                     if key not in ("id", "name", "sources")}})
        binding["inputs"] = inputs
    compatibility = _bytes({"format": "vehicle-profile-compatibility-v1",
                            "profile_id": profile["profile_id"],
                            "semantic_policy_revision": profile["semantic_policy_revision"],
                            "selected_bus": profile["selected_bus"], "bindings": bindings})
    artifact = _bytes({"format": "vehicle-profile-artifact-v1",
                       "generator_revision": generator_revision,
                       "profile": normalized, "validation": _validation(policies, validation_limits)})
    profile["compatibility_identity"] = hashlib.sha256(compatibility).hexdigest()
    profile["artifact_identity"] = hashlib.sha256(artifact).hexdigest()
    return ProfileIdentity(profile, compatibility, artifact)
