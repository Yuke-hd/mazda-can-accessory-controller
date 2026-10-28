"""Minimal derived `ValidationPolicies`: a build product of `mapping.json`.

Imported profiles have no native handlers, no hand-written registry and no
freshness entries here (host tools only).
"""

from .dbc_mapping import mapping_revision, semantic_revision
from .validation import ValidationPolicies

EVIDENCE_CONFIDENCE = "user_mapped"
EVIDENCE_ELIGIBILITY = "source_only"


def evidence_ref(profile_id: str) -> str:
    return "mapping:" + profile_id


def derive_registry(mapping: dict) -> ValidationPolicies:
    """Derive the registry the validator needs from a shape-checked mapping."""
    profile_id = mapping["profile_id"]
    return ValidationPolicies(
        profile_revisions={profile_id: (semantic_revision(mapping), mapping_revision(mapping))},
        handlers={},
        evidence=frozenset({(evidence_ref(profile_id), EVIDENCE_CONFIDENCE,
                             EVIDENCE_ELIGIBILITY)}),
        units=frozenset(mapping["units"]),
        unit_conversions={(source, target): (scale, offset)
                          for source, target, scale, offset in mapping["unit_conversions"]},
        invalid_value_policies=frozenset({"unavailable"}),
        freshness_policies={})
