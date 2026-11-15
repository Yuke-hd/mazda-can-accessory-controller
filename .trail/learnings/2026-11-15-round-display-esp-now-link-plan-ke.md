---
title: Round display ESP-NOW link plan (controller side)
author: Ke
date: 2026-11-15
branch: claude/round-display-communication-e8a47b
cost: { minutes: 7, tokens: 102358 }
---

## Decided
- The round display repo (esp32-1-75) owns the wire contract: docs/protocol/esp-now-v1.md (200-byte ECAN v1, 23 stable IDs, 20 Hz, encrypted native ESP-NOW) and pairing-v1.md (display = Security1 initiator, controller = responder, espressif/esp-now 2.5.3 + display's SDK overlay, LMK = first 16 bytes of the 32-byte key).
- Owner 2026-11-15: one milestone for sender + pairing; CONFIG_DISPLAY_ESPNOW opt-in (default off) coexisting with BLE companion; ~5 s user-key long press reboots into setup mode (short press stays BLE window); setup code written to NVS at flash time by a host script.
- Encoder goes in lib/companion_protocol mirroring LiveSignalSampler (key-resolved, read()-only); transport in new components/display_espnow mirroring companion_ble::start()/mark_telemetry_started().

## Tried and ruled out
- Reusing the BLE live frame layout: it has 21 slots and different codes; the display needs its own 23-ID table.
- Display/BLE numeric SignalIds on the wire: per-build handles, never transmitted.

## Watch out
- Contract rejects Fresh for brake IDs 19/23; stopped packets must be all Unknown with zero values.
- Vendor the display's SDK overlay unchanged (SHA-checked apply.py); copies can drift across repos.
- BLE+Wi-Fi coexistence RAM/flash on classic ESP32 is unmeasured; measure before any default-on.
