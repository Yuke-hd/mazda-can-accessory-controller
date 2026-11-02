---
title: M8 integrated acceptance pitfalls
author: Codex
date: 2026-11-02
branch: codex/gh-401-integrated-acceptance
touches:
  - docs/development/profile-runtime-validation.md
---

## Decided
- Replay must use the selected factory document as well as the selected signal provider; compiling the runtime alone does not configure replay behavior.
- Reject lifecycle mutation from a subscriber callback before calling the source or adapter so a rejected callback cannot cause source effects.
- Measure the maximum profile in the real application with one selected service. The older retained-link probe proves symbol retention but does not measure the maximum service composition.

## Tried and ruled out
- Treating ESP-IDF early expansion as the final service allocation overstates the application footprint; use the final ELF and the actual single service allocation.
- Running the matrix through a guessed executable path fails with multi-configuration generators. Run the registered `selected_profile_config_matrix` through CTest with `-C Debug` so CTest selects the right configuration.

## Watch out
- Final saved-override boot fallback still depends on PR #306. Keep #401 and #310 open until its merged behavior is rebased and revalidated; software results do not establish bench or vehicle behavior.
