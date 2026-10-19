---
title: DBC vehicle support intent and evidence boundary
author: Ke
date: 2026-10-18
branch: main
touches:
  - docs/intent/dbc-vehicle-support.md
  - docs/specs/dbc-vehicle-support.md
---

## Decided
The owner confirmed that importing a DBC must automatically map controller inputs without manual assignment. Uncertain mappings disable only affected features. Build-time import is an acceptable first milestone if profiles and validation carry forward to owner configuration with little rework. Mazda remains the regression baseline; the second real vehicle is intentionally undecided. See docs/intent/dbc-vehicle-support.md.

## Tried and ruled out
Manual owner mapping was explicitly rejected. Universal DBC recognition is not promised. Runtime upload, owner UI, and profile switching are excluded from the initial milestone. The draft specification is proposed, not approved implementation authority.

## Watch out
The current Mazda DBC comparison tool is not a general importer, and compiled Mazda definitions remain authoritative. Automatic mapping must not invent signal meaning, timing evidence, or vehicle validation. The existing Mazda brake freshness exception must not automatically propagate to other vehicles. Preserve strict configuration validation when disabling unsupported features.
