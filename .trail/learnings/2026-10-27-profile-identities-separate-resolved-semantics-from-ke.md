---
title: Profile identities separate resolved semantics from reproducibility
author: Ke
date: 2026-10-27
branch: codex/gh-315-identity
touches:
  - docs/specs/vehicle-profiles/v1-format.md
  - tests/fixtures/vehicle-profiles/identity-golden.json
  - tests/tools/vehicle_profile_identity_test.py
  - tools/vehicle_profiles/canonical.py
---

## Decided
Version-one compatibility bytes resolve exposed binding inputs to message/field mechanics and include canonical choices, reviewed handler revisions, evidence, and freshness. Native input argument order is significant. Artifact bytes cover the normalized full profile, source byte digests and attribution, recognition/generator revisions, and the full supplied validation registry. Both preimages exclude their supplied digest fields and source paths; exact decimal strings normalize without floating-point or decimal-context rounding.

## Tried and ruled out
Hashing the input JSON text directly would make numeric spellings and source ordering change identity. Hashing only catalog keys would miss changed scale or meaning. Sorting native input arrays would change handler argument meaning. Literal independently authored empty/native golden preimages establish the byte encoding separately from production serialization.

## Watch out
The host identity stage validates normalized data and policy references; it does not add DBC import, recognition, or firmware runtime support. Trail may lexically confuse the identity spec's references to two preimages or numeric offset with prior notes about native handler arity or tuple offsets. These identity definitions preserve those earlier boundaries: validation composes aliases before handler checks and no tuple bookkeeping is added.
