---
title: Mazda subscriptions bind by stable channel identity
author: Ke
date: 2026-10-27
branch: codex/gh-319-registration-identity
touches:
  - components/mazda_telemetry/private_include/mazda/signal_catalog.hpp
  - components/mazda_telemetry/private_include/mazda/signal_subscription_record.hpp
  - components/mazda_telemetry/private_include/mazda/vehicle_telemetry_service.hpp
  - components/mazda_telemetry/src/vehicle_telemetry.cpp
  - components/mazda_telemetry/tests/signal_catalog_tests.cpp
  - components/mazda_telemetry/tests/vehicle_telemetry_descriptor_tests.cpp
  - components/mazda_telemetry/tests/vehicle_telemetry_group_tests.cpp
  - docs/architecture/signal-layering.md
  - docs/development/adding-a-signal.md
  - tests/host/replay_signal_observer_tests.cpp
---

## Decided
Private subscription wrappers and host notification descriptor tests select NotificationDescriptor types using their stable NotificationChannel alias. std::get by descriptor type rejects missing or duplicate channel types at compile time and adds no runtime registry. Catalog size and generic subscriber capacity derive from their definitions. Independent signal_catalog_tests expectations for released keys, IDs, choices, wire values and per-channel capacities remain explicit.

## Tried and ruled out
Tuple offsets and production count-only assertions required unrelated edits when adding a descriptor. Keep message-group evaluation expectations in the profiling group test independent, but locate the RearLeftDoor, Turn and Brake descriptors by channel identity.

## Watch out
Do not renumber existing channel IDs, including the host-only extension channel. Host group-count tests require MAZDA_ENABLE_TELEMETRY_PROFILING=ON for the descriptor evaluation hook; enable MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING as well when testing stage metrics. Firmware checks must retain the exact post-link IRAM gate. Software builds and tests establish no hardware or vehicle validation.
