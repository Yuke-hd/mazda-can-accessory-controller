---
title: "Output catalog generator and CMake wiring (#281)"
author: Ke
date: 2026-10-18
branch: claude/agent-skills-build-c6d563
cost: { minutes: 6, tokens: 138683 }
touches:
  - CMakeLists.txt
  - firmware/weact-can485-v1.1/CMakeLists.txt
  - components/local_argb_actions/output_catalog.yaml
  - firmware/weact-can485-v1.1/output_catalog.cmake
  - tests/tools/generate_output_catalog_test.py
  - tools/generate_output_catalog.py
  - tools/output_catalog.cmake
  - docs/specs/companion/output-catalog.md
  - tests/host/CMakeLists.txt
---

## Decided
- Generation logic lives in one shared CMake module, tools/output_catalog.cmake (output_catalog_generate(BOARD_FILE OUTPUT_DIR)); root CMakeLists.txt and firmware/weact-can485-v1.1/CMakeLists.txt both call it with the WeAct board file, so host and firmware compile the same header. It defines target `output_catalog` (ALL) and sets OUTPUT_CATALOG_JSON / OUTPUT_CATALOG_INCLUDE_DIR for later tickets (#282 controller_config include dir + add_dependencies, #283 compiler --output-catalog).
- The generator builds both outputs in memory and writes them via temp files + os.replace, so errors write nothing and leave old outputs intact.
- tests/tools/generate_output_catalog_test.py reads the 1518-byte reference vector straight from docs/specs/companion/output-catalog.md, so the spec stays the single source of truth.
- Extra guards beyond the spec: field names whose PascalCase collides (a_b vs a__b) and C++-keyword output types are rejected because they would break the header.

## Watch out
- tests/tools/*_test.py run in CI via `unittest discover`, not CTest; the new test is not registered in tests/host/CMakeLists.txt (tickets 3-5 also edit that file).
- The YAML loader drops YAML 1.1 yes/no/on/off booleans and rejects duplicate keys, mirroring compile_controller_config.py.
