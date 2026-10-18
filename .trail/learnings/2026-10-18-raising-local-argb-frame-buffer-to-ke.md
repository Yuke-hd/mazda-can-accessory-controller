---
title: Raising local_argb frame buffer to 300 with 100 active (GH-289)
author: Ke
date: 2026-10-18
branch: claude/issue-289-3b290c
cost: { minutes: 12, tokens: 132273 }
touches:
  - components/board/include/board/board_config.h
  - components/controller_config/src/persisted/validation.cpp
  - components/local_argb/include/local_argb/local_argb.h
  - components/local_argb/private_include/local_argb/led_zone.hpp
  - components/local_argb/private_include/local_argb/renderer.hpp
  - components/local_argb/src/led_zone.cpp
  - components/local_argb/src/local_argb.cpp
  - components/local_argb/src/local_argb_idf.cpp
  - components/local_argb/tests/public_boundary.py
  - components/local_argb/tests/rendering_tests.cpp
---

## Decided
- PixelFrameSink::write takes (frame, length). Lit frame = kDefaultLedCount; every all-black frame = kMaxLedCount, so length is a function of content and renderer dedupe is unchanged.
- Renderer working frames are RendererController members (static via g_controller), not stack locals.
- Replay JSONL clamps to kDefaultLedCount so replay output is byte-identical.
- Factory layout constants derive from kDefaultLedCount (brake start (L-30)/2, right turn L-35).

## Tried and ruled out
- Firmware build: `idf.py -B /tmp/x build` compiled all components (local_argb_idf.cpp included) but failed at the bootloader subproject CMake configure (requirements parse error); env issue, not verified on main.

## Watch out
- Tests reading frame.back() or frame.size()==100 must use kDefaultLedCount.
- #282 check kZonePixels == kLedCount must become kDefaultLedCount (output-catalog.md:234 still says kLedCount).
- tools/validate_* grep for the literal "kVehicleLightStripPixelCount = 300".
