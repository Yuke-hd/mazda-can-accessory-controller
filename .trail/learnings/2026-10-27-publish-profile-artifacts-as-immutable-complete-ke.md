---
title: Publish profile artifacts as immutable complete generations
author: Ke
date: 2026-10-27
branch: codex/gh-317-artifacts
touches:
  - tools/vehicle_profiles/artifact_set.py
  - tools/generate_vehicle_profile.py
  - tools/vehicle_profile.cmake
  - tests/tools/vehicle_profile_artifact_set_test.py
  - docs/development/supported-build.md
---

## Decided
Issue #317 publishes immutable content-addressed sets and switches one current.json index only after every output is written and verified. Consumers resolve one immutable directory for all catalog/header/report reads. CMake records all source/policy/manifest/generator dependencies and generates with Python alone; the direct CLI --check is the non-mutating drift interface. The inventory uses the existing normalized profile/policy identity, retaining source bytes and generator hashes without raw-JSON formatting sensitivity.

## Tried and ruled out
Replacing outputs one by one permits mixed generations after a late failure. A CMake check target cannot promise non-mutation when automatic reconfigure regenerates inputs first. Neither a new decoder nor a runtime profile-selection path is part of artifact publication.

## Watch out
Keep old immutable directories while consumers may still read them. This provides atomic process-failure publication, not a power-loss durability or garbage-collection policy. Trail's lexical matches against earlier runtime, IRAM, raw JSON hashing and historical SDK failures do not describe this offline publication implementation; preserve all those earlier boundaries. A final-file injected failure is tested after an existing successful generation, not just in an empty directory.
