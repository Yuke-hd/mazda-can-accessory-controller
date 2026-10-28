---
title: Reject escaped and empty corrupt artifact sets
author: Ke
date: 2026-10-28
branch: codex/gh-317-publication-hardening
touches:
  - tools/vehicle_profiles/artifact_set.py
  - tests/tools/vehicle_profile_artifact_set_test.py
---

## Decided
Reject a symlinked sets directory before checking or publishing artifacts. Reject any existing mismatched generation, including an empty directory or generation symlink, before staging and rename. A valid concurrent same-digest publication remains reusable. Failure preserves the current index, previous reader bytes, and external directories.

## Tried and ruled out
Checking only CMake's output-root containment misses a symlink inside that root. Relying on rename failure to detect corrupt generations misses empty directories, which directory rename can replace. Red regressions reproduced both failures; direct publication and real CMake tests now guard them.

## Watch out
These checks cover observed filesystem state, not adversarial concurrent path replacement or power-loss durability. Keep corrupt generations for investigation and use a fresh output root for recovery. Existing publication/build-rule learnings remain applicable; lexical matches to runtime, handler, identity, IRAM, or SDK notes do not imply this offline filesystem fix changes those contracts.
