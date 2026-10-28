---
title: DBC importer contract preserves exact source values and context
author: Ke
date: 2026-10-27
branch: codex/gh-321-import-spec
touches:
  - docs/specs/vehicle-profiles/dbc-import.md
  - docs/README.md
---

## Decided
Issue #321 proposes a bounded offline importer contract for owner review. It keeps exact numeric values, source byte digests and locations, selected-bus conflict detection, explicit automatic recognition, and model-scoped handlers. The resolved recognition revision content-addresses pack id/revision and the exact context tuple so artifact identity retains recognition inputs without extending the v1 root shape. The proposed initial parser is a constrained repository-owned lossless tokenizer; this documentation adds no production dependency or runtime behavior.

## Tried and ruled out
The current Mazda comparison regex parser is a regression checker, not a complete import grammar. Primary-source evaluation of cantools 45.1.0 at 68131411d777da8024e11ec79ddb7bebe92c2da0 found the ordinary DBC numeric loader uses int/float, so that model alone cannot establish exact decimal import. The private parser API was not adopted. canmatrix 1.3.3.1 was evaluated as an alternative requiring an adapter/conformance proof; no upstream code or DBC fixture was copied.

## Watch out
The D1 specification remains proposed until owner review. Its source lexer grammar is separate from normalized-v1 numeric strings; legal BO_ lengths are 0–64 with only standard classic 0–8 executable. Native handler input aliases normalize before arity checks and preserve argument order. Trail may lexically flag this future-module specification against A3's narrower structural-slice boundary; the PR explains that scope distinction rather than changing prior learnings.
