# Repository scaffold (MCAN-3)

This work item records the initial repository and build-boundary milestone.
It is historical context; current host and firmware build instructions are
maintained in [development documentation](../development/supported-build.md)
and [firmware builds](../development/firmware-build.md).

## Initial scope

The scaffold established the portable host library and ESP32 firmware
structure. It intentionally did not decode Mazda signals, record captures,
export telemetry, or provide a CAN data-generation path. The original board
component recorded the WeAct board capabilities and fail-closed electrical
defaults; see the current [board capability record](../architecture/hardware/weact-can485-v1.1.md).

The initial host library targeted C++17. The firmware manifest pinned ESP-IDF
`v5.5.4` rather than downloading an arbitrary SDK during configuration. The
host tests pinned doctest to an exact commit. That version choice addressed
the old `v2.4.11` CMake metadata's pre-3.5 minimum, which is rejected by CMake
4. Current supported versions and dependency details live in the development
guides linked above.

The milestone's CI checks were compile and configuration evidence only, not
bench or vehicle validation. Dependency source, exact current versions, and
upstream license links are recorded in
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).
