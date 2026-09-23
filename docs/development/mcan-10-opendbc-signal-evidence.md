# MCAN-10 Mazda signal evidence

Status: the reviewed capture-derived DBC is committed at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc). Its #51
definitions below are confirmed for the listed signals; the upstream matrix
remains an unverified source of unrelated candidate signals.

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
vendored in this repository. The separate capture-derived source artifact for
#51 is committed at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc); it is not an
opendbc distribution. No vehicle capture is included. This document is a
provenance record and candidate lead for the upstream matrix, not a broader
compatibility claim. Build and test tooling has no opendbc download or
floating-branch input.

## Upstream candidate matrix

The matrix below is retained as historical provenance for the existing
opendbc-derived leads, including out-of-scope fields. The capture-derived
definitions in the following section supersede overlapping candidates for the
confirmed #51 signals only.

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
| `WHEEL_SPEEDS` (533 / `0x215`) | `FL`: `7|16@0+ (0.01,-100)`, `[0,16383]`, kph; `FR`: `23|16@0+ (0.01,-100)`, `[0,65535]`, kph; `RL`: `39|16@0+ (0.01,-100)`, `[0,15]`, kph; `RR`: `55|16@0+ (0.01,-100)`, `[0,65535]`, kph | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L73-L77) | Wheel naming, scaling, and bus placement may vary by market and ECU software | Upstream candidate — unverified for Australian target |
| `GEAR` (552 / `0x228`) | `NEW_SIGNAL_3`: `11|1@0+ (1,0)`, `[0,1]`; `NEW_SIGNAL_5`: `26|3@0+ (1,0)`, `[0,255]`; `NEW_SIGNAL_6`: `31|5@0+ (1,0)`, `[0,31]`; `NEW_SIGNAL_7`: `39|1@0+ (1,0)`, `[0,255]`; `MORE_GEAR`: `7|4@0+ (1,0)`, `[0,15]`; `GEAR`: `2|3@0+ (1,0)`, `[0,7]`; `GEAR_BOX`: `36|4@0+ (1,0)`, `[0,15]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L488-L496) | Upstream comment maps `GEAR` values to P/R/N/D, but this is not Australian-vehicle evidence; retain unknown values | Upstream candidate — unverified for Australian target |
| `TURN_SWITCH` (145 / `0x091`) | `HAZARD`: `10|1@0+ (1,0)`, `[0,1]`; `TURN_RIGHT_SWITCH`: `12|1@0+ (1,0)`, `[0,3]`; `TURN_LEFT_SWITCH`: `13|1@0+ (1,0)`, `[0,255]`; `CTR`: `27|4@0+ (1,0)`, `[0,255]`; `CHKSUM`: `39|8@0+ (1,0)`, `[0,15]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L436-L442) | Switch semantics and counter/checksum behavior require Australian-target evidence; upstream `carstate.py` does not consume this message | Upstream candidate — unverified for Australian target |
| `BLINK_INFO` (154 / `0x09A`) | `LEFT_BLINK`: `18|1@1+ (1,0)`, `[0,3]`; `RIGHT_BLINK`: `19|1@0+ (1,0)`, `[0,255]`; `REAR_WIPER_ON`: `0|1@0+ (1,0)`, `[0,1]`; `WIPER_LO`: `33|1@1+ (1,0)`, `[0,31]`; `WIPER_HI`: `34|1@0+ (1,0)`, `[0,1]`; `LOW_BEAMS`: `5|2@0+ (1,0)`, `[0,3]`; `HIGH_BEAMS`: `7|2@0+ (1,0)`, `[0,3]`; `LBEAM1`: `17|1@0+ (1,0)`, `[0,1]`; `LBEAM2`: `50|1@0+ (1,0)`, `[0,1]`; `LBEAM3`: `60|1@0+ (1,0)`, `[0,1]` | Unspecified by source; establish from reviewed replay | [`opendbc/dbc/mazda_2017.dbc`](https://github.com/commaai/opendbc/blob/95f3d52f474b677c28fc8f10fef3f2f0386aff92/opendbc/dbc/mazda_2017.dbc#L424-L434) | Lamp and wiper availability/semantics may vary by market, trim, and body controller; upstream `carstate.py` consumes only selected lamp fields | Upstream candidate — unverified for Australian target |

The source's unusual limits are reproduced verbatim. These are DBC-declared
ranges, not vehicle observations; a field's representable bit width may be
broader or narrower than its declared range. Values must not be silently
normalized before local evidence exists.

## Capture-derived confirmed definitions

The following definitions were reviewed from the capture-derived DBC supplied
for #51. They are limited to the confirmed signals in that ticket. The DBC
start-bit notation, byte order, scale, offset, range, and value table are
recorded here so the allocation-free decoder can be checked without bundling a
DBC parser. The reviewed source file is committed at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc); its
reference-only fields remain outside the confirmed decoder scope.

| Message (hex ID) | Signal | DBC definition | Confirmed value table |
| --- | --- | --- | --- |
| `ENGINE_DATA` (`0x202`) | `EngineRPM` | `7\|16@0+ (0.25,0) [0\|8500] rpm` | physical rpm |
| `TRANSMISSION` (`0x228`) | `Selector` | `2\|3@0+ (1,0) [0\|7]` | `0=Shifting`, `1=Park`, `2=Reverse`, `3=Neutral`, `4=Drive`, `5..7=Unknown` |
| `TRANSMISSION` (`0x228`) | `ActualGear` | `36\|4@0+ (1,0) [0\|15]` | `0=P_or_N`, `1..6=1st..6th`, `7..13=Unknown`, `14=Reverse`, `15=Shifting` |
| `DOORS` (`0x43E`) | `Liftgate_Open` | `32\|1@0+ (1,0) [0\|1]` | `0=Closed`, `1=Open` |
| `DOORS` (`0x43E`) | `RearRightDoor_Open` | `34\|1@0+ (1,0) [0\|1]` | `0=Closed`, `1=Open` |
| `DOORS` (`0x43E`) | `RearLeftDoor_Open` | `35\|1@0+ (1,0) [0\|1]` | `0=Closed`, `1=Open` |
| `DOORS` (`0x43E`) | `FrontLeftDoor_Open_RHD` | `36\|1@0+ (1,0) [0\|1]` | `0=Closed`, `1=Open` |
| `DOORS` (`0x43E`) | `FrontRightDoor_Open_RHD` | `37\|1@0+ (1,0) [0\|1]` | `0=Closed`, `1=Open` |
| `DOORS` (`0x43E`) | `DoorsUnlocked` | `30\|1@0+ (1,0) [0\|1]` | `0=Locked`, `1=Unlocked` |
| `BLINK_INFO` (`0x09A`) | `LeftIndicatorLamp` | `18\|1@1+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `BLINK_INFO` (`0x09A`) | `RightIndicatorLamp` | `19\|1@0+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `BLINK_INFO` (`0x09A`) | `WiperLow` | `33\|1@0+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `TURN_SWITCH` (`0x091`) | `HazardSwitch` | `10\|1@0+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `TURN_SWITCH` (`0x091`) | `RightIndicatorSwitch` | `12\|1@0+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `TURN_SWITCH` (`0x091`) | `LeftIndicatorSwitch` | `13\|1@0+ (1,0) [0\|1]` | `0=Off`, `1=On` |
| `TURN_SWITCH` (`0x091`) | `FrontWiper` | `21\|2@0+ (1,0) [0\|3]` | `0=Off`, `1=On`, `2=High`, `3=Intermittent` |

The legacy front-door reference label from the supplied DBC is represented as
the confirmed right-hand-drive semantic signal `FrontLeftDoor_Open_RHD`. No
confirmed signal in this section retains a reference suffix. The
`Selector` and `ActualGear` fields remain independent semantic values even
though they share the `TRANSMISSION` message.

## Local verification boundary

| Definition class | Local evidence in this change | Status |
| --- | --- | --- |
| Listed capture-derived IDs, field positions, scaling, units, values, and signal names | Reviewed capture-derived DBC; no raw capture, VIN, location, absolute time, or vehicle-derived fixture is included | Confirmed for the listed signals |
| Signal periods and freshness periods | No cycle-time declaration in the supplied DBC | Unspecified; preserve existing turn/request freshness policy only |
| Australian-market 2019 CX-5 Akera compatibility beyond the listed definitions | None | Unknown; do not claim broader compatibility |
| Upstream candidate signals outside #51 | Existing opendbc matrix above | Unverified and unchanged |

Future validation of timing, broader compatibility, and the unrelated
candidate definitions must use the receive-only staged procedure and a
reviewed, privacy-safe fixture or isolated-bench evidence. The listed signal
mappings are confirmed; no active probing, diagnostic polling, CAN
transmission, or vehicle release artifact is in scope for MCAN-10.
