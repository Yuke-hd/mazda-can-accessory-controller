---
title: "M8 integration spec aligned to simplified plan (I0 #393)"
author: Ke
date: 2026-11-02
branch: claude/ponytail-full-implementation-d80cf0
cost: { minutes: 2, tokens: 91056 }
touches:
  - docs/specs/vehicle-profiles/profile-integration.md
  - tools/import_vehicle_profile.py
---

## Decided
- profile-integration.md now reflects #310's simplified M8 plan: VEHICLE_PROFILE is `native:mazda` (default) or a mapping.json path, resolved against the repository root in host and ESP-IDF builds.
- Imported builds reuse tools/import_vehicle_profile.py output (controller_signal_catalog.json, metadata/runtime headers, capability report) as the single generation; compile_controller_config.py gets the selected catalog as an explicit input instead of its fixed checked-in path.
- Factory prunes unsupported built-in features completely; imported defaults use `fresh` only, so no declared timeout_ms means black output. Saved rules validate directly against the selected provider's public signal contract; imported selections reject fresh_or_unverified.

## Tried and ruled out
- Build-request JSON, recognition/policy packs, selection descriptor, VPROFOV1 storage envelope, legacy Mazda bridge, startup identity comparison, factory-JSON transaction: all removed from the spec.

## Watch out
- `trail check` flags ~14 keyword "conflicts" on this spec (Kconfig matrix, legacy bridge, firmware build). They are false positives: the spec names those concepts only to exclude them, or describes M8 scope #310 assigns. Explain in the PR body.
- Final boot integration waits for PR #306; do not copy its implementation.
