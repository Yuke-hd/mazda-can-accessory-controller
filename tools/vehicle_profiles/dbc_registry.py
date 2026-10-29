"""Minimal derived `ValidationPolicies`: a build product of `mapping.json`.

Imported profiles have no native handlers and no hand-written registry. Unit
conversions come from the bindings' resolved (field unit, binding unit) pairs;
freshness rows exist only for bindings that declare `timeout_ms` (host tools only).
"""

from typing import Iterable

from .dbc_mapping import conversion_of, mapping_revision, semantic_revision
from .validation import ValidationPolicies

EVIDENCE_CONFIDENCE = "user_mapped"
ELIGIBILITY_SOURCE = "source_only"
ELIGIBILITY_OPERATIONAL = "operational"
FRESHNESS_BASIS = "reviewed_operational"
EVIDENCE_ELIGIBILITY = ELIGIBILITY_SOURCE


def evidence_ref(profile_id: str) -> str:
    return "mapping:" + profile_id


def conversion_groups(pairs: Iterable[tuple[dict, str]]) -> dict:
    """Group (binding, field unit) pairs by resolved unit pair: pair -> {target: (scale, offset)}.

    Scale and offset are canonical decimal texts, so equal rationals compare equal.
    """
    groups: dict[tuple[str, str], dict[str, tuple[str, str]]] = {}
    for binding, field_unit in pairs:
        groups.setdefault((field_unit, binding["unit"]), {})[binding["target"]] = (
            conversion_of(binding))
    return groups


def conflicts(groups: dict) -> list[tuple[tuple[str, str], list[str]]]:
    """Unit pairs whose bindings disagree, with every target in the group, sorted."""
    return [(pair, sorted(members)) for pair, members in sorted(groups.items())
            if len(set(members.values())) > 1]


def derive_registry(mapping: dict, field_units: dict[str, str]) -> ValidationPolicies:
    """Derive the registry for a shape-checked, fully resolved mapping.

    `field_units` maps each binding target to its DBC field's unit.
    """
    profile_id = mapping["profile_id"]
    bindings = mapping["bindings"]
    groups = conversion_groups((binding, field_units[binding["target"]]) for binding in bindings)
    if conflicts(groups):
        raise ValueError("conflicting unit conversions")
    ref = evidence_ref(profile_id)
    evidence = {(ref, EVIDENCE_CONFIDENCE, ELIGIBILITY_SOURCE)}
    freshness = {(profile_id, binding["target"]): (binding["timeout_ms"], FRESHNESS_BASIS, ref)
                 for binding in bindings if "timeout_ms" in binding}
    if freshness:
        evidence.add((ref, EVIDENCE_CONFIDENCE, ELIGIBILITY_OPERATIONAL))
    return ValidationPolicies(
        profile_revisions={profile_id: (semantic_revision(mapping), mapping_revision(mapping))},
        handlers={},
        evidence=frozenset(evidence),
        units=frozenset(binding["unit"] for binding in bindings),
        unit_conversions={pair: next(iter(members.values())) for pair, members in groups.items()},
        invalid_value_policies=frozenset({"unavailable"}),
        freshness_policies=freshness)
