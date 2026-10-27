---
title: Profile composition preserves native handler argument meaning
author: Ke
date: 2026-10-27
branch: codex/gh-314-mechanics
touches:
  - tools/vehicle_profiles/validation.py
  - tests/tools/vehicle_profile_validation_test.py
  - tools/vehicle_profiles/bit_coordinates.py
---

## Decided
Normalized profile validation composes equivalent DBC fields and retains all source references. Native handler inputs remain positional: alias normalization happens before signature validation, and a binding with two source identities for one logical field is rejected rather than silently shrinking its argument list. The reviewed policy registry scopes handler revisions to explicit profiles and operational freshness to an explicit profile/input tuple; null freshness stays unset.

## Tried and ruled out
Checking handler arity before field deduplication allowed a valid-looking two-input binding to become a one-input binding in the returned profile. Sorting native input identities also risks changing positional meaning. Both are covered by an independent regression and revalidation check.

## Watch out
The schema and synthetic validation fixtures establish host contracts only. They do not implement DBC import, recognition, firmware runtime, or vehicle validation. Canonical identities must preserve native input order while making provenance and source composition deterministic.
