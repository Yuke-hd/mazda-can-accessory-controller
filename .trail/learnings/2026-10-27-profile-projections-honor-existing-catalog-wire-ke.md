---
title: Profile projections honor existing catalog wire limits
author: Ke
date: 2026-10-27
branch: codex/gh-316-projections
touches:
  - tests/fixtures/vehicle-profiles/projection-empty-expected.json
  - tests/fixtures/vehicle-profiles/projection-river-expected.json
  - tests/tools/vehicle_profile_catalog_probe.cpp
  - tests/tools/vehicle_profile_projection_test.py
  - tools/vehicle_profiles/projections.py
---

## Decided
Issue #316 projects one A4-validated/A5-identified profile into compiler JSON, static SignalMetadata, and capability diagnostics. All outputs derive from the same sorted bindings. Existing export limits remain 64 signals, 32 choices per enum, 63-byte keys, 31-byte tokens, and 16384 document bytes. Unsupported canonical units/confidence fail explicitly. Empty profiles emit an empty catalog and an all-features-disabled report. The trusted Mazda catalog-only adapter wraps compiler metadata without inventing extraction or freshness.

## Tried and ruled out
Partial/truncated catalogs and implicit unit or confidence conversions would make the consumer views disagree. Instead, a real C++ SignalCatalogView/SignalCatalogExport probe checks generated metadata, exact document boundary and overflow, enum provider values, and empty output. Host projection code introduces no DBC parser, recognition engine, generated provider runtime, or firmware placement changes.

## Watch out
The projection tool tests configure a no-tests host build to locate the exact pinned core header, then compile the real catalog/export code. They do not execute target firmware. Trail's lexical matches against older schema/identity/IRAM learnings concern shared words in this layer; the implementation delegates semantic validation and identity preimages to the unchanged A4/A5 APIs, and preserves their boundaries.
