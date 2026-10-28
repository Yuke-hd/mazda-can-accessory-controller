---
title: Normalized profile schema preserves the DBC recognition boundary
author: Ke
date: 2026-10-27
branch: codex/gh-313-schema
touches:
  - docs/specs/vehicle-profiles/v1-format.md
  - tests/fixtures/vehicle-profiles/synthetic-empty.json
  - tests/fixtures/vehicle-profiles/synthetic-river.json
  - tests/tools/vehicle_profile_manifest_test.py
  - tools/vehicle_profiles/__init__.py
  - tools/vehicle_profiles/model.py
  - tools/vehicle_profiles/validation.py
---

## Decided
Issue #313 publishes only the normalized version-one host schema, strict structural validation, and independently authored synthetic fixtures. The recognition_rule_revision field records a future reviewed rule revision; it does not implement recognition. Native and DBC binding variants describe normalized data and retain the distinction between extraction and reviewed native handlers.

## Tried and ruled out
Universal DBC recognition remains unpromised. No DBC parser, automatic mapping engine, firmware profile runtime, or vehicle support is implemented or claimed by this structural slice. Placeholder fixture hashes establish record shape only.

## Watch out
The existing DBC intent learning may produce lexical trail-check conflicts when the schema mentions DBC and recognition metadata. For this slice those are false positives: review the executable scope and preserve the prior evidence and recognition boundaries. Mechanical/policy validation and computed identities belong to dependent tickets.
