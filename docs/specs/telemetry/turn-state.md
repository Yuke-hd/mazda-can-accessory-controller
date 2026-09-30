# Turn and hazard state

For a valid `TURN_SWITCH` (`0x091`) frame, the decoder extracts the
`HazardSwitch`, `RightIndicatorSwitch`, and `LeftIndicatorSwitch` fields using
byte/LSB numbering (byte 1 bits 2, 4, and 5), updates the three request
signals, decodes `FrontWiper` from byte 2 bits 5..4, and normalizes the switch
state to `TurnState`. Frame acceptance and malformed-frame handling are
specified in [telemetry decoder behaviour](decoder-behaviour.md); field
coordinates are in [Mazda decoder mappings](../../protocol/decoder-mappings.md).

## Normalization

| HAZARD | LEFT | RIGHT | normalized state |
| --- | --- | --- | --- |
| 1 | either | either | Hazard |
| 0 | 1 | 0 | Left |
| 0 | 0 | 1 | Right |
| 0 | 1 | 1 | Unknown (conflict) |
| 0 | 0 | 0 | Off |

## Freshness and edge events

Hazard therefore takes precedence over both directions. A fresh frame keeps
the state valid for 250,000 microseconds; after more than 250,000
microseconds without an accepted update, the signal is `Stale`. Unknown and
stale states are non-actionable: `VehicleState::effective_turn_state()` returns
`Unknown`, which gives indicator consumers fail-off semantics. The stored raw
value is retained for diagnostics while its status is stale.

`mazda::VehicleState::update_turn()` emits a `mazda::TurnEdgeEvent` only when the semantic
state changes. Equal states and duplicate frames do not create duplicate
edges. The decoder can optionally write that event to an output
`std::optional<mazda::TurnEdgeEvent>`, which is cleared for ignored, invalid, and
duplicate frames. Before comparing states, `update_turn()` refreshes the
mutable turn signal at the incoming timestamp. A stale value is therefore not
actionable, and a recovered frame produces an edge from effective `Unknown`,
including when it recovers to the same stored direction.

## Related decoded statuses

`BLINK_INFO` (`0x09A`) decodes left and right indicator lamp status from byte
2 bits 2 and 3, and `WiperLow` from byte 4 bit 1. It does not alter request or
normalized turn state. The reviewed DBC is at
[mazda_custom.dbc](../../protocol/mazda_custom.dbc); the per-channel evidence
assessment is in [Mazda signal evidence](../../protocol/signal-evidence.md).
The DBC has no cycle-time declaration, so no source timing or additional
freshness threshold is inferred here. Tests use synthetic frames and a
simulated monotonic replay clock; no capture or vehicle-derived data is
included. Decoder boundaries are documented in
[telemetry decoder behaviour](decoder-behaviour.md).
