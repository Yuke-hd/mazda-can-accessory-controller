# MCAN-71 DBC metadata verification

Status: Stage 2 S2-D host verification for the S1-E evidence inventory.

The typed constexpr definitions in
`lib/mazda/include/mazda/definitions.hpp`
remain the executable metadata authority. The reviewed source artifact
`docs/protocol/mazda_custom.dbc` remains
byte-for-byte source evidence. S2-D does not parse DBC at runtime, generate a
runtime schema, or change decoder numeric/semantic definitions.

## Host check

Run the comparison from the repository root:

```text
python3 tools/compare_mazda_dbc.py
```

The checker compiles
`tools/dump_mazda_metadata.cpp`, which
exports the compiled constexpr table in a stable tab-delimited host-tool
format. The checker then compares the supported 17-signal subset's stable
public channel, identifier, decoder-facing start bit, DBC start bit, length,
byte order, scale, offset, and declared physical range. Channel, coordinate,
and numeric mismatches are errors; they cannot be hidden by an evidence
exception.

The two start-bit coordinates are intentionally distinct. For Intel (`@1`)
signals, the DBC start bit is already the least-significant payload bit, so it
is also the decoder-facing coordinate. For Motorola (`@0`) signals, the DBC
start bit identifies the most-significant bit in the sawtooth byte order; the
checker walks that sequence (wrapping from bit 0 to bit 7 of the next byte) and
exports the lowest ordinary byte/LSB payload coordinate used by the decoder.
For example, `EngineRPM` `7|16@0` maps to decoder bit `0`, `ActualGear`
`36|4@0` maps to `33`, and Intel `LeftIndicatorLamp_Reference` `18|1@1`
remains `18`.

`SPEED` is intentionally excluded because it is a retained candidate and has
no field in the reviewed custom DBC. The only source-name/interpretation
exception is `DOORS.FrontOtherDoor_Open_Reference` mapped to the public
`front_left_door_open_rhd` channel. Reference suffix removal on other public
names is represented explicitly in the constexpr supported-signal map.

## Evidence boundary

Each supported metadata entry carries a `ValidationStatus` confidence and a
provenance reference. The assignments are the S1-E inventory: `EngineRPM`,
`Selector`, and `FrontRightDoor_Open_RHD` are `Confirmed`; `ActualGear`,
`WiperLow`, and `FrontWiper` are `Observed`; the remaining supported entries
are `Reference`. These statuses describe reviewed evidence, not decoder
validity, freshness, or runtime availability.

The module-local
`dbc_metadata_comparison_test.py`
checks the reviewed baseline, verifies the DBC digest, and mutates temporary
metadata to prove that channel, decoder-coordinate, and numeric drift plus an
unsupported confidence promotion fail with diagnostics. Temporary mutations
are never written to the repository.
