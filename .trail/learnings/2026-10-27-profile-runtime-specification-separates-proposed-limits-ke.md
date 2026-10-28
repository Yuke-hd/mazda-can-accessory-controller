---
title: Profile runtime specification separates proposed limits from measured evidence
author: Ke
date: 2026-10-27
branch: codex/gh-322-runtime-spec
touches:
  - docs/specs/vehicle-profiles/profile-runtime.md
---

## Decided
Issue #322 proposes the generated runtime contract: one receive-only standard classic-CAN input, existing SignalProvider consumer boundary, pure reviewed handlers, coherent copied publication, strict newer-observation recovery, explicit per-profile freshness and Mazda brake isolation. Existing two-slot subscriber, catalog, queue and opaque-storage bounds remain reference constraints. New generated ceilings are proposed for owner review and require actual production/profiling resource evidence before implementation acceptance.

## Tried and ruled out
A specification cannot claim runtime implementation, vehicle support or resource measurements. Historical firmware figures do not validate a future generated provider. Missing profile policy cannot inherit Mazda timing, enums or the brake exception; imported text cannot become firmware code.

## Watch out
Trail's lexical checker may flag this specification because it mentions words in prior failure notes. The document preserves the relevant decisions: aliases normalize before handler arity checks; runtime ids are local; compatibility hashing remains host-owned; no transmission or manual field mapping is introduced. EasyEDA and prior bootloader environment notes concern separate systems or historical evidence. Owner review is required before runtime coding.
