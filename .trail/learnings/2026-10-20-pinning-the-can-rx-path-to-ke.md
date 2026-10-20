---
title: Pinning the can_rx path to IRAM (GH-235)
author: Ke
date: 2026-10-20
branch: perf/can-rx-iram
touches:
  - docs/development/build-modes.md
  - docs/development/firmware-build.md
  - docs/development/flash-cache-layout.md
  - firmware/weact-can485-v1.1/CMakeLists.txt
  - firmware/weact-can485-v1.1/main/hot_code.lf
  - firmware/weact-can485-v1.1/sdkconfig.defaults
---

## Decided
- Extended main/hot_code.lf with exact-symbol noflash entries for can_bus receive_task and receive, vehicle_telemetry CanBusSource::receive, the TWAI task API wrappers plus their _v2 implementations, and twai_hal_parse_frame/get_tec/get_rec.
- Enabled CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH in sdkconfig.defaults. The firmware uses can_bus's custom SPSC FrameRing, not the IDF ringbuf; non-ISR ringbuf helpers can be in flash while ISR entry points stay in IRAM.
- Fresh ESP-IDF 5.5.4 out-of-tree builds of production, worst-case profiling, and synthetic configurations all link. The worst-case profiling image keeps 705 B of IRAM free after freeing about 792 B of ringbuf IRAM.

## Tried and ruled out
- Pinning the receive path without moving non-ISR ringbuf functions to flash overflowed the worst-case profiling IRAM region by 88 B.
- Leaving the TWAI API and HAL parser in flash would leave the otherwise IRAM-resident receive loop with cache-sensitive callees, so those exact symbols are included.

## Watch out
- The worst-case IRAM headroom is now under 1 KiB; rebuild all three configurations before adding hot symbols or IRAM-consuming options.
- A deliberately corrupted can_bus symbol makes the POST_BUILD checker fail as intended; the exact names must stay clone-free.
- The earlier note about an out-of-tree firmware build failing at the bootloader CMake configure was environment-specific. On this macOS host, idf.py -C firmware/weact-can485-v1.1 -B /tmp/<dir> -D SDKCONFIG=/tmp/<dir>/sdkconfig set-target esp32 followed by build succeeded for all three configurations.
