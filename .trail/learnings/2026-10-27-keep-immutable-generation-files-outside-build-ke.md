---
title: Keep immutable generation files outside build-rule deletion
author: Ke
date: 2026-10-27
branch: codex/gh-317-artifacts
touches:
  - tools/vehicle_profiles/artifact_set.py
  - tools/vehicle_profile.cmake
  - tests/tools/vehicle_profile_artifact_set_test.py
---

## Decided
A CMake generation prerequisite is phony and publication is a no-op for an already matching set. Immutable set files and current.json are never declared as outputs of a failing build rule: Make can delete those files when generation fails, including an unchanged previous current.json. Missing or corrupt immutable files fail closed with a fresh-output-root recovery diagnostic; readers' existing directories remain untouched.

## Tried and ruled out
The initial CMake OUTPUT list included current.json and the generated header. A missing header made generation reject the nonempty corrupt directory, then Make deleted the previous index as a failed target output. A regression now verifies the old index bytes survive that exact path. Lexical build-directory containment also allowed a symlinked parent to write outside the build root.

## Watch out
Resolve both the build root and output path, including existing parent symlinks, before generating anything. The escape regression uses a nonexistent child under a symlink; CMake REAL_PATH alone does not resolve that case, so the module uses Python Path.resolve. Fresh-root recovery is explicit; automatic repair must not silently replace a generation while readers use it.
