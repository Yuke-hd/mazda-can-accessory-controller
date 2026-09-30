# Signal definition review (historical)

> This is the preserved MCAN-10 review record. Its blanket “confirmed” wording
> predates the current per-channel evidence assessment and is not authoritative
> for confidence. Consult [Mazda signal evidence](../protocol/signal-evidence.md)
> for current per-channel assignments, and [opendbc provenance](../protocol/opendbc-provenance.md)
> for the upstream candidate matrix and source pin.

The review material below is retained to preserve the original field inventory,
local verification boundary, and planned evidence limitations. Historical
issue and stage references describe the review at that time.

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
| Upstream candidate signals outside #51 | [opendbc provenance matrix](../protocol/opendbc-provenance.md) | Unverified and unchanged |

Future validation of timing, broader compatibility, and the unrelated
candidate definitions must use the receive-only staged procedure and a
reviewed, privacy-safe fixture or isolated-bench evidence. The listed signal
mappings are confirmed; no active probing, diagnostic polling, CAN
transmission, or vehicle release artifact is in scope for MCAN-10.
