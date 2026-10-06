# opendbc Mazda candidate provenance

This document records the exact upstream source behind the Mazda candidate
matrix below. Every matrix entry remains an upstream lead, unverified for the
Australian target. The reviewed project DBC and current per-channel confidence
assignments are documented separately in [the DBC](mazda_custom.dbc) and
[Mazda signal evidence](signal-evidence.md).

## Pin and provenance

The candidate source is comma.ai/opendbc at the exact commit
`95f3d52f474b677c28fc8f10fef3f2f0386aff92`, accessed 2026-08-16. The commit was
resolved from `refs/heads/master`; the branch is not a dependency. The source
repository is [comma.ai/opendbc](https://github.com/commaai/opendbc), licensed
under MIT. The upstream notice is `Copyright (c) 2020, Comma.ai, Inc.` and the
exact permission and disclaimer text is preserved in
[`third_party/licenses/opendbc-MIT.txt`](../../third_party/licenses/opendbc-MIT.txt),
with the attribution index in
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).

The extraction reads these paths at that commit:

- [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc)
  for message IDs and field definitions;
- [`opendbc/car/mazda/carstate.py`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/car/mazda/carstate.py)
  for the upstream consumer references; and
- [`opendbc/car/mazda/values.py`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/car/mazda/values.py)
  and [`docs/CARS.md`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/docs/CARS.md)
  for the upstream model-scope lead.

The candidate matrix is an opendbc-derived field extraction distributed as
project documentation. No full opendbc source tree or upstream DBC file is
vendored in this repository. The reviewed project DBC is a separate artifact;
no vehicle capture is included. This page is a provenance record and candidate
lead, not a broader compatibility claim. Build and test tooling has no opendbc
download or floating-branch input.

## Upstream candidate matrix

The matrix records opendbc-derived leads, including fields outside the
current decoder scope. All rows remain unverified for the Australian target;
current channel confidence is tracked in
[Mazda signal evidence](signal-evidence.md).

The field syntax below preserves the DBC definition: `start|length@endian`
followed by `(scale,offset)`, limits, and unit. `@0` is Motorola/big-endian
and `@1` is Intel/little-endian. The source DBC contains no `GenMsgCycleTime`
or equivalent period declaration for these messages. Therefore the expected
period is deliberately **unspecified** until a reviewed, privacy-safe replay
or isolated-bench observation establishes it; no decoder freshness threshold
may be inferred from this table.

Every row is an **upstream candidate — unverified for the Australian target**.
The target is an Australian-market 2019 Mazda CX-5 Akera; regional, trim,
powertrain, ECU software, and bus differences remain open questions. No row
is locally verified, and no row authorizes active probing, transmission, or a
claim of vehicle compatibility.

| Message (decimal / hex ID) | Candidate field definitions from `mazda_2017.dbc` | Expected period | Source | Market caveat | Validation status |
| --- | --- | --- | --- | --- | --- |
| `ENGINE_DATA` (514 / `0x202`) | `CHKSUM`: `63|8@0+ (1,0)`, `[0,127]`; `RPM`: `7|16@0+ (0.25,0)`, `[0,8500]`, rpm; `SPEED`: `23|16@0+ (0.01,0)`, `[0,32767]`, kph; `PEDAL_GAS`: `39|12@0+ (1,0)`, `[0,255]`, % | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L53-L57) | Mazda 2017 DBC lead; Australian 2019 CX-5 Akera not established | Upstream candidate — unverified for Australian target |
| `BRAKE` (120 / `0x078`) | `VEHICLE_ACC_X`: `5|13@0+ (0.01,-40)`, `[-40|40]`, m/s^2; `VEHICLE_ACC_Y`: `8|13@0+ (0.001,-4.096)`, `[-4.096|4.096]`, m/s^2 | Unspecified by source; establish from an annotated validation capture | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L356-L360), source comments [X](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L778) and [Y](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L779) | Upstream Mazda 2017 lead; the Australian 2019 CX-5 Akera capture contains the frame but does not establish axis, sign, or acceleration scaling | Upstream candidate — unverified for Australian target |
| `WHEEL_SPEEDS` (533 / `0x215`) | `FL`: `7|16@0+ (0.01,-100)`, `[0,16383]`, kph; `FR`: `23|16@0+ (0.01,-100)`, `[0,65535]`, kph; `RL`: `39|16@0+ (0.01,-100)`, `[0,15]`, kph; `RR`: `55|16@0+ (0.01,-100)`, `[0,65535]`, kph | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L73-L77) | Wheel naming, scaling, and bus placement may vary by market and ECU software | Upstream candidate — unverified for Australian target |
| `GEAR` (552 / `0x228`) | `NEW_SIGNAL_3`: `11|1@0+ (1,0)`, `[0,1]`; `NEW_SIGNAL_5`: `26|3@0+ (1,0)`, `[0,255]`; `NEW_SIGNAL_6`: `31|5@0+ (1,0)`, `[0,31]`; `NEW_SIGNAL_7`: `39|1@0+ (1,0)`, `[0,255]`; `MORE_GEAR`: `7|4@0+ (1,0)`, `[0,15]`; `GEAR`: `2|3@0+ (1,0)`, `[0,7]`; `GEAR_BOX`: `36|4@0+ (1,0)`, `[0,15]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L488-L496) | Upstream comment maps `GEAR` values to P/R/N/D, but this is not Australian-vehicle evidence; retain unknown values | Upstream candidate — unverified for Australian target |
| `TURN_SWITCH` (145 / `0x091`) | `HAZARD`: `10|1@0+ (1,0)`, `[0,1]`; `TURN_RIGHT_SWITCH`: `12|1@0+ (1,0)`, `[0,3]`; `TURN_LEFT_SWITCH`: `13|1@0+ (1,0)`, `[0,255]`; `CTR`: `27|4@0+ (1,0)`, `[0,255]`; `CHKSUM`: `39|8@0+ (1,0)`, `[0,15]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L436-L442) | Switch semantics and counter/checksum behavior require Australian-target evidence; upstream `carstate.py` does not consume this message | Upstream candidate — unverified for Australian target |
| `BLINK_INFO` (154 / `0x09A`) | `LEFT_BLINK`: `18|1@1+ (1,0)`, `[0,3]`; `RIGHT_BLINK`: `19|1@0+ (1,0)`, `[0,255]`; `REAR_WIPER_ON`: `0|1@0+ (1,0)`, `[0,1]`; `WIPER_LO`: `33|1@1+ (1,0)`, `[0,31]`; `WIPER_HI`: `34|1@0+ (1,0)`, `[0,1]`; `LOW_BEAMS`: `5|2@0+ (1,0)`, `[0,3]`; `HIGH_BEAMS`: `7|2@0+ (1,0)`, `[0,3]`; `LBEAM1`: `17|1@0+ (1,0)`, `[0,1]`; `LBEAM2`: `50|1@0+ (1,0)`, `[0,1]`; `LBEAM3`: `60|1@0+ (1,0)`, `[0,1]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L424-L434) | Lamp and wiper availability/semantics may vary by market, trim, and body controller; upstream `carstate.py` consumes only selected lamp fields | Upstream candidate — unverified for Australian target |

The source's unusual limits are reproduced verbatim. These are DBC-declared
ranges, not vehicle observations; a field's representable bit width may be
broader or narrower than its declared range. Values must not be silently
normalized before local evidence exists.

The original MCAN-10 capture-derived inventory and its superseded blanket
confidence wording remain in the
[historical signal definition review](../work-items/signal-definition-review.md).
