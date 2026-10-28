---
title: Profile integration binds overrides at the private storage boundary
author: Ke
date: 2026-10-27
branch: codex/gh-323-integration
touches:
  - docs/specs/vehicle-profiles/profile-integration.md
  - docs/README.md
---

## Decided
Issue #323 is a proposed integration contract, gated on owner review before coding. One build-selected profile supplies runtime, compiler, factory, replay and BLE catalog artifacts through one immutable A7 generation. New persisted saves bind canonical version-one JSON to exact profile id and semantic compatibility identity in a private storage envelope; public companion version-one document and wire bytes remain unchanged. The integration profile-id limit is 1–63 ASCII bytes, validated before generation so every selection fits storage. Bare legacy JSON is accepted only by an explicit owner-reviewed exact native-Mazda compatibility allowlist.

## Tried and ruled out
Adding profile keys to today's strict version-one config would change the public format. An unconditional legacy bridge or key-overlap inference could accept another profile's old settings. A claim that StorageFailed preserves persisted selection overstates the current backend: running configuration stays unchanged, while a committed marker and failed rollback can leave old, verified-new or factory selection next boot. Acceptance explicitly covers that uncertainty and same-session upload/revert recovery.

## Watch out
The document is a proposal with future implementation/resource gates, not importer/runtime/vehicle validation. Trail's lexical check can flag its explicit Kconfig rejection, atomic-publication requirement, identity separation and host/firmware acceptance references against prior scope learnings; these references preserve those boundaries. Apply-time fallback follows separate PR #306 and must be rebased onto its accepted behavior rather than reimplemented here.
