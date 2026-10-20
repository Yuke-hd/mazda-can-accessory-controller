---
title: Final can_rx IRAM set and trail-check false positive
author: Ke
date: 2026-10-20
branch: perf/can-rx-iram
touches:
  - docs/development/flash-cache-layout.md
  - firmware/weact-can485-v1.1/main/hot_code.lf
---

## Decided
- Final #235 set: receive_task, can_bus::receive, CanBusSource::receive, RawCanFrame::is_valid, the TWAI receive/read_alerts/get_status wrappers plus their _v2 implementations, and twai_hal_parse_frame. Move non-ISR IDF ringbuf helpers to flash.
- Do not pin twai_hal_get_tec/rec: twai_get_status_info_v2 only calls them outside listen-only mode.
- Final headroom: production 125,703/5,369 B IRAM used/free; worst-case profiling 130,411/661 B; synthetic 74,363/56,709 B.

## Tried and ruled out
- Pinning the earlier 36-symbol set without moving ringbuf helpers overflowed the worst-case image by 88 B.
- Pinning get_tec/rec wasted 30 B and led to a false claim that RawCanFrame::is_valid was inlined.
- Relying only on the task entry leaves RawCanFrame::is_valid as a cross-TU flash call.

## Watch out
- RawCanFrame::is_valid must be pinned explicitly; it is not inlined into receive_task.
- CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH also moves non-ISR UART ringbuf helpers, not only an unused ring; keep ISR entry points in IRAM.
- trail check flags this change as bringing back receive flash from the earlier 88 B learning. That is a lexical false positive: this change includes the ringbuf move that learning requires.
