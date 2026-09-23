# Mazda signal evidence inventory

Status: Stage 1 S1-E inventory for the frozen telemetry contracts.

This document is the project-owned evidence boundary for the public Mazda
channels. It records source field mappings and confidence without adding a
runtime schema. The typed constexpr definitions remain the executable metadata
authority; the reviewed DBC remains source evidence at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc).

## Confidence criteria

The status below is evidence confidence, not runtime availability. It is
independent of `SignalStatus`, `Availability`, and freshness policy.

| Status | Meaning | What it does not mean |
| --- | --- | --- |
| **Reference** | A source-derived field or project interpretation with no documented matching vehicle observation. | It is not a validated vehicle signal. |
| **Observed** | A documented correlation or partially exercised mapping whose complete semantics, enum coverage, or range is not established. | It is not confirmation of every value or scale claim. |
| **Confirmed** | A reviewed repository evidence reference supports the specific field interpretation and returned value/scale. | It does not confirm unobserved enum values, timing, market compatibility, or a vehicle limit. |

Synthetic decoder vectors and host tests establish implementation consistency
with the mapping. They do not promote a Reference or Observed assignment.
Historical documents use some blanket “confirmed” wording; this per-channel
inventory is the current evidence assignment and resolves that ambiguity
without changing those historical records or the frozen decoder metadata.

## Public channel inventory

The public facade declares two polling channels and sixteen notify channels in
[`mazda/vehicle_telemetry.hpp`](../../components/mazda_telemetry/include/mazda/vehicle_telemetry.hpp).
The corresponding state members are listed in
[`VehicleState`](../../lib/mazda/include/mazda/state.hpp#L11-L34).
“DBC source” uses the byte-for-byte reviewed file; “decoder” identifies the
current portable implementation and metadata. A status qualified by a value
set applies only to that set, not to unobserved values in the same field.
The private lighting handoff additionally consumes the confirmed brake-pedal
state; it is not exposed as a public facade channel.

| Public channel | Mode | DBC source field / derivation | Decoder and source mapping | Evidence status and boundary |
| --- | --- | --- | --- | --- |
| `speed_kph` | Polling | Retained `SPEED` candidate; no `SPEED` field is present in the reviewed custom DBC. | `kEngineSpeedDefinition` in [`definitions.hpp`](../../lib/mazda/include/mazda/definitions.hpp#L112-L122), with metadata physical maximum and representable 16-bit maximum of 655.35 km/h; consumed from `ENGINE_DATA` bytes 2–3 in [`mazda_candidate.cpp`](../../lib/mazda/src/mazda_candidate.cpp#L86-L105). | **Reference** — inherited upstream candidate; synthetic vectors do not establish vehicle evidence. |
| `engine_rpm` | Polling | `ENGINE_DATA.EngineRPM`, ID `0x202`, `7\|16@0+`, scale `0.25`, offset `0`, source range `[0,8500]`. | Metadata [`kEngineRpmDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L97-L111); big-endian decode and raw `>34000` rejection in [`mazda_candidate.cpp`](../../lib/mazda/src/mazda_candidate.cpp#L86-L105). | **Confirmed** for field position, scale, and returned RPM interpretation by the reviewed DBC evidence ([DBC comment](../protocol/mazda_custom.dbc#L94-L96)); the source range is declared, not a measured vehicle limit. |
| `selector_position` | Notify | `TRANSMISSION.Selector`, ID `0x228`, `2\|3@0+`, scale `1`, value table `0=Shifting`, `1=Park`, `2=Reverse`, `3=Neutral`, `4=Drive`, `5..7=Unknown`. | Metadata [`kSelectorDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L123-L137); `data[0] & 0x07` in [`decode_gear`](../../lib/mazda/src/mazda_candidate.cpp#L113-L137). | **Confirmed** for the reviewed P/R/N/D interpretation ([DBC comment](../protocol/mazda_custom.dbc#L97-L99)); raw `0` and values `5..7` retain source labels and are not vehicle-confirmed. |
| `actual_gear` | Notify | `TRANSMISSION.ActualGear`, ID `0x228`, `36\|4@0+`, scale `1`, value table `0=P_or_N`, `1..6=1st..6th`, `7..13=Unknown`, `14=Reverse`, `15=Shifting`. | Metadata [`kActualGearDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L138-L154); `(data[4] >> 1) & 0x0f` in [`decode_gear`](../../lib/mazda/src/mazda_candidate.cpp#L113-L137). | **Observed** — the reviewed DBC records the `1st <-> 2nd` correlation ([DBC comment](../protocol/mazda_custom.dbc#L97-L99)); raw `15=Shifting` is typed and accepted but unobserved, while raw `7..13` remain undefined. Other values, including `P_or_N` and reverse, remain source mappings. |
| `turn_state` | Notify | Derived semantic state from the three `TURN_SWITCH` request fields; not a direct DBC field. | `normalize_turn()` and `VehicleState::update_turn()` in [`mazda_candidate.cpp`](../../lib/mazda/src/mazda_candidate.cpp#L72-L82) and [`state.cpp`](../../lib/mazda/src/state.cpp#L7-L23); precedence is documented in [`mcan-15-turn-state.md`](mcan-15-turn-state.md#L23-L36). | **Reference** — project-derived normalization is covered by synthetic tests, not a separate vehicle evidence record. |
| `hazard_request` | Notify | `TURN_SWITCH.HazardSwitch_Reference`, ID `0x091`, `10\|1@0+`, `0=Off`, `1=On`. | Metadata [`kHazardDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L183-L184); byte 1 bit 2 in [`decode_turn_switch`](../../lib/mazda/src/mazda_candidate.cpp#L197-L225). | **Reference** — source mapping only; the reviewed DBC retains the `_Reference` suffix ([DBC](../protocol/mazda_custom.dbc#L58-L61)). |
| `left_turn_request` | Notify | `TURN_SWITCH.LeftIndicatorSwitch_Reference`, ID `0x091`, `13\|1@0+`, `0=Off`, `1=On`. | Byte 1 bit 5 in [`decode_turn_switch`](../../lib/mazda/src/mazda_candidate.cpp#L197-L225). | **Reference** — source mapping only; normalization and synthetic edges do not promote field evidence. |
| `right_turn_request` | Notify | `TURN_SWITCH.RightIndicatorSwitch_Reference`, ID `0x091`, `12\|1@0+`, `0=Off`, `1=On`. | Byte 1 bit 4 in [`decode_turn_switch`](../../lib/mazda/src/mazda_candidate.cpp#L197-L225). | **Reference** — source mapping only; the source field remains explicitly reference-labelled ([DBC](../protocol/mazda_custom.dbc#L58-L61)). |
| `liftgate_open` | Notify | `DOORS.Liftgate_Open_Reference`, ID `0x43e`, `32\|1@0+`, `0=Closed`, `1=Open`. | Metadata [`kLiftgateOpenDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L163-L164); byte 4 bit 0 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Reference** — the source comment says this field was not empirically tested ([DBC](../protocol/mazda_custom.dbc#L100-L106)). |
| `rear_right_door_open` | Notify | `DOORS.RearRightDoor_Open_Reference`, ID `0x43e`, `34\|1@0+`, `0=Closed`, `1=Open`. | Metadata [`kRearRightDoorOpenDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L165-L166); byte 4 bit 2 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Reference** — source mapping only; no matching vehicle observation is recorded ([DBC](../protocol/mazda_custom.dbc#L100-L106)). |
| `rear_left_door_open` | Notify | `DOORS.RearLeftDoor_Open_Reference`, ID `0x43e`, `35\|1@0+`, `0=Closed`, `1=Open`. | Metadata [`kRearLeftDoorOpenDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L167-L168); byte 4 bit 3 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Reference** — source mapping only; no matching vehicle observation is recorded ([DBC](../protocol/mazda_custom.dbc#L100-L106)). |
| `front_left_door_open_rhd` | Notify | Current semantic interpretation of `DOORS.FrontOtherDoor_Open_Reference`, ID `0x43e`, `36\|1@0+`, `0=Closed`, `1=Open`. | Metadata [`kFrontLeftDoorOpenRhdDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L169-L172); byte 4 bit 4 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Reference** — the source calls this bit `FrontOtherDoor` and explicitly marks it reference-only ([DBC](../protocol/mazda_custom.dbc#L45-L50), [comment](../protocol/mazda_custom.dbc#L100-L105)). The `_RHD` public name is a current interpretation, not confirmation. |
| `front_right_door_open_rhd` | Notify | `DOORS.FrontRightDoor_Open_RHD`, ID `0x43e`, `37\|1@0+`, `0=Closed`, `1=Open`. | Metadata [`kFrontRightDoorOpenRhdDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L173-L174); byte 4 bit 5 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Confirmed** for the reviewed RHD field/bit/value interpretation ([DBC comment](../protocol/mazda_custom.dbc#L100-L102)); broader trim/market compatibility is not established. |
| `doors_unlocked` | Notify | `DOORS.DoorsUnlocked_Reference`, ID `0x43e`, `30\|1@0+`, `0=Locked`, `1=Unlocked`. | Metadata [`kDoorsUnlockedDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L175-L176); byte 3 bit 6 in [`decode_doors`](../../lib/mazda/src/mazda_candidate.cpp#L139-L169). | **Reference** — source comment records a reference definition and conflicting supplied captures ([DBC](../protocol/mazda_custom.dbc#L100-L106)). |
| `left_indicator_lamp` | Notify | `BLINK_INFO.LeftIndicatorLamp_Reference`, ID `0x09a`, `18\|1@1+`, `0=Off`, `1=On`. | Metadata [`kLeftIndicatorLampDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L177-L178); byte 2 bit 2 in [`decode_blink_info`](../../lib/mazda/src/mazda_candidate.cpp#L171-L195). | **Reference** — the source field is reference-labelled and no signal-specific observation is recorded ([DBC](../protocol/mazda_custom.dbc#L53-L56), [value table](../protocol/mazda_custom.dbc#L85-L86)). |
| `right_indicator_lamp` | Notify | `BLINK_INFO.RightIndicatorLamp_Reference`, ID `0x09a`, `19\|1@0+`, `0=Off`, `1=On`. | Metadata [`kRightIndicatorLampDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L179-L180); byte 2 bit 3 in [`decode_blink_info`](../../lib/mazda/src/mazda_candidate.cpp#L171-L195). | **Reference** — source mapping only; synthetic lamp tests do not promote it ([DBC](../protocol/mazda_custom.dbc#L53-L56)). |
| `wiper_low` | Notify | `BLINK_INFO.WiperLow_Reference`, ID `0x09a`, `33\|1@0+`, `0=Off`, `1=On`. | Metadata [`kWiperLowDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L181-L182); byte 4 bit 1 in [`decode_blink_info`](../../lib/mazda/src/mazda_candidate.cpp#L171-L195). | **Observed** — the reviewed DBC records correlation with wiper activity but explicitly describes the interpretation as “appears” to be operation rather than a stalk request ([DBC comment](../protocol/mazda_custom.dbc#L107-L108)). |
| `front_wiper` | Notify | `TURN_SWITCH.FrontWiper`, ID `0x091`, `21\|2@0+`, scale `1`, value table `0=Off`, `1=On`, `2=High`, `3=Intermittent`. | Metadata [`kFrontWiperDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L192-L206); `(data[2] >> 4) & 0x03` in [`decode_turn_switch`](../../lib/mazda/src/mazda_candidate.cpp#L197-L225). | **Observed** — the reviewed DBC records the field/bit correlation and observed `0=Off`, `1=On`; values `2=High`, `3=Intermittent` remain reference mappings ([DBC comment](../protocol/mazda_custom.dbc#L109-L110)). |
| `brake_pressed` | Private lighting input | `BRAKE_PEDAL.BrakePressed`, ID `0x165`, `4\|1@0+`, `0=Released`, `1=Pressed`. | Metadata [`kBrakePressedDefinition`](../../lib/mazda/include/mazda/definitions.hpp#L246-L248); `data[0] & 0x10` in [`decode_brake_pedal`](../../lib/mazda/src/mazda_candidate.cpp#L347-L371). | **Confirmed** for the reviewed field/bit/value interpretation ([DBC comment](../protocol/mazda_custom.dbc#L111-L112)); no public polling or notification channel is added. |

The metadata line links above are intentionally descriptive only. If a future
layout split moves a definition, S2-D should preserve the stable channel name,
DBC field, and this status boundary rather than treating a source-line move as
new evidence.

## Source names and semantic differences

The reviewed DBC deliberately retains source caution in names such as
`*_Reference`. Public channel names remove that suffix where the frozen API
requires a stable application-facing name; the evidence status remains
Reference unless a separate reviewed observation supports the interpretation.

The front-door mapping is the important exception to keep visible:

| Source field | Current public channel | Difference and confidence |
| --- | --- | --- |
| `FrontOtherDoor_Open_Reference` at bit 36 | `front_left_door_open_rhd` | The decoder currently exposes a left-door RHD interpretation for this source “other door” bit. It remains **Reference**; no source comment confirms it. |
| `FrontRightDoor_Open_RHD` at bit 37 | `front_right_door_open_rhd` | The DBC comment records a reviewed RHD field/bit/value correlation. It is **Confirmed** for that specific interpretation. |

`Selector` and `ActualGear` share `TRANSMISSION` but are independent semantic
channels. `turn_state` is a normalized project state derived from request bits;
it must not be read as independent evidence for those source switch fields.
The `speed_kph` channel is retained for compatibility with the existing
candidate decoder even though the reviewed custom DBC contains no `SPEED`
signal.

## Partial enumeration and scaling coverage

The status of a field does not silently promote values that were not observed.

| Channel | Source-declared values/range | Locally represented values | Evidence boundary |
| --- | --- | --- | --- |
| `engine_rpm` | Unsigned 16-bit raw, scale `0.25`, source range `0..8500 rpm`. | Decoder accepts raw `0..34000`; the bit width can represent raw `0..65535` (`0..16383.75 rpm`) but values above the source-declared maximum are rejected. | `0.25` field interpretation is Confirmed; the source range is not a measured vehicle limit. |
| `speed_kph` | Existing candidate scale `0.01` on an unsigned 16-bit field. | Candidate metadata physical maximum and encoded representable maximum are both **655.35 km/h** (`65535 * 0.01`). | Reference only; `655.35 km/h` is an encoding ceiling, not a validated speed limit. |
| `selector_position` | Three-bit unsigned `0..7`; table names `Shifting`, P/R/N/D, and `Unknown_5..7`. | All eight source codes have a typed representation; only the P/R/N/D transition interpretation is Confirmed. | `0` and `5..7` are not promoted by the P/R/N/D evidence. |
| `actual_gear` | Four-bit unsigned `0..15`; `0=P_or_N`, `1..6` gears, `7..13=Unknown`, `14=Reverse`, `15=Shifting`. | Decoder accepts semantic mappings for `0`, `1..6`, `14`, and `15=Shifting`; only raw `7..13` are undefined and invalidate the affected signal. | Observed evidence is limited to the documented `1st <-> 2nd` correlation; raw `15` is typed/accepted but remains unobserved vehicle evidence. |
| Door/lamp/request booleans | One-bit unsigned `0..1`, with source value tables. | Both boolean values are represented by the decoder. | Synthetic coverage is not vehicle evidence; statuses remain those in the inventory. |
| `front_wiper` | Two-bit unsigned `0..3`, table Off/On/High/Intermittent. | All four codes have typed representations. | `0=Off` and `1=On` are observed; `2=High` and `3=Intermittent` remain Reference mappings. |

The DBC has no `GenMsgCycleTime` or equivalent period declaration. No message
period, signal period, or freshness timeout is inferred from the tables above.
The existing 250,000 us turn/request freshness policy is an operational
policy, not evidence that the source emits at that interval.

## Evidence and attribution references

- The reviewed source artifact is [`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc); its bytes are unchanged by S1-E. Signal comments at [lines 94–110](../protocol/mazda_custom.dbc#L94-L110) are cited only as repository evidence; no raw capture or private trip data is reproduced here.
- [`mcan-10-opendbc-signal-evidence.md`](mcan-10-opendbc-signal-evidence.md) records the exact upstream opendbc commit, MIT attribution, candidate matrix, and the earlier capture-derived field inventory. Upstream rows remain candidates until matching target evidence exists.
- [`mcan-14-candidate-decoders.md`](mcan-14-candidate-decoders.md) records the decoder locations, field coordinates, scale/offset, synthetic vectors, and their privacy boundary. Those synthetic vectors are implementation tests, not evidence promotion.
- [`mcan-15-turn-state.md`](mcan-15-turn-state.md) records the project-owned turn normalization and fail-off semantics; it does not add vehicle evidence for the source switch fields.
- The earlier blanket-confidence table in [`mcan-10-opendbc-signal-evidence.md`](mcan-10-opendbc-signal-evidence.md#L74-L116) is retained as historical provenance; this inventory is the per-channel correction and is not a new vehicle capture.

No runtime provenance database, generated signal schema, new signal research,
new capture, credential, or private vehicle artifact is part of this inventory.

## S2-D metadata verification

The supported DBC subset is checked against the compiled constexpr metadata
authority by the host-only
[MCAN-71 comparison tool](mcan-71-dbc-metadata.md). The tool covers the
stable public channel, decoder/DBC bit coordinates, numeric fields, and the
explicit source-name/interpretation exception recorded above; an actual
channel or coordinate/numeric mismatch fails. It does not promote evidence
based on synthetic tests or runtime availability.
