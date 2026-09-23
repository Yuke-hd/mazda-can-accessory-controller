# MCAN-15 normalized turn and hazard state

`mazda::candidate::decode_turn_switch()` accepts only a valid,
standard, non-RTR classic-CAN frame with ID `0x091` and DLC 8. It extracts the
confirmed `HazardSwitch`, `RightIndicatorSwitch`, and `LeftIndicatorSwitch`
fields using byte/LSB numbering (byte 1 bits 2, 4, and 5 respectively),
updates the three request signals, decodes `FrontWiper` from byte 2 bits 5..4,
and normalizes the switch state to `TurnState`:

| HAZARD | LEFT | RIGHT | normalized state |
| --- | --- | --- | --- |
| 1 | either | either | Hazard |
| 0 | 1 | 0 | Left |
| 0 | 0 | 1 | Right |
| 0 | 1 | 1 | Unknown (conflict) |
| 0 | 0 | 0 | Off |

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

`BLINK_INFO` (`0x09A`) is also decoded as confirmed status: the left and right
indicator lamps use byte 2 bits 2 and 3, and `WiperLow` uses byte 4 bit 1. It
does not alter request or normalized turn state. These definitions are
capture-derived from the reviewed DBC committed at
[`docs/protocol/mazda_custom.dbc`](../protocol/mazda_custom.dbc); signal timing
remains unspecified because the DBC has no cycle-time declaration. Tests use
synthetic frames and a simulated monotonic replay clock; no capture or
vehicle-derived data is included. The decoder only receives frames and writes
in-memory state; it has no CAN transmit or vehicle-control interface.
