# MCAN-4 portable domain model

The portable `vehicle_core` headers define the low-level boundary shared by
host tools and device firmware. They are C++17 and have no Mazda decoder,
ESP-IDF, Arduino, FreeRTOS, transport, display, or LED dependency. Mazda
semantic state and model types are owned by `lib/mazda`; see
[`mcan-17-module-boundaries.md`](mcan-17-module-boundaries.md) for the header
map and migration notes.

## Raw frames

`RawCanFrame` is a value type for a classic CAN receive record. It retains the
monotonic timestamp, `bus_id`, identifier and standard/extended format, RTR
flag, DLC, and an eight-byte fixed payload. The fixed array is intentional:
frames can be copied through a bounded queue without transferring ownership or
allocating. CAN-FD payloads are outside this API. `is_valid()` checks the
classic-CAN DLC and identifier limits; it does not claim that a frame was
observed on a particular Mazda network.

There is no transmit operation, CAN driver handle, or raw-frame forwarding
interface in this component.

## Signals and freshness

`vehicle_core::Signal<T>` stores a value, explicit `has_value` bit, generic
`SignalUnit`, last-update monotonic timestamp, and `SignalStatus` (`Unknown`,
`Valid`, or `Stale`). Status is explicit, so a valid zero speed or RPM is never
confused with an uninitialized value. `update()` rejects an older timestamp and
conflicting equal-time values; identical equal-time observations are
idempotent. Each signal stores its own bounded freshness timeout. The pure
`status_at(now)`/`snapshot(now)` helpers clamp backwards time to age zero,
preserve retained values, and report `FreshnessUnverified` when no timeout
evidence is configured. The mutating `refresh(now)` compatibility path still
produces a conservative raw `Stale` view for legacy callers. Unknown signals
remain `NoData` until their first update; an explicitly invalidated retained
value is `Unavailable`. The initial turn/request policy is 250 ms.

`mazda::VehicleState` provides speed, RPM, selector position, actual transmission
gear, liftgate/door state, central unlock state, indicator-lamp state,
low-speed wiper state, front-wiper selection, turn, hazard, left-turn, and
right-turn signal slots. Selector position and actual gear are separate value
signals and cannot overwrite or alias one another. More signals can be added
without introducing transport or board types. `snapshot(now)` evaluates each
signal's own freshness policy on a value copy and leaves the source state
unchanged. `reading_at(signal, identifier, now)` additionally applies the
relevant message and transport health, retaining a last value while reporting
`Unavailable`. A `VehicleFreshnessPolicy` can supply synthetic or later
verified per-signal timeouts without adding dynamic storage or asserting
unverified production defaults. Fixed per-message records reject older or
conflicting frames, latch relevant malformed faults, and clear them only on a
strictly newer valid frame.

## Semantic events and time

`mazda::TurnEdgeEvent` contains only semantic previous/current turn states and a
monotonic timestamp. It deliberately has no CAN ID or payload. A
`vehicle_core::MonotonicClock` adapter supplies time, while `mazda::VehicleStateStore` owns one
state by value and provides deterministic snapshots through the
`SnapshotProvider` interface. Host tests can provide a fixed clock; firmware
can adapt its monotonic timer without changing the domain model.

All ownership is explicit and non-owning at interfaces: `mazda::VehicleStateStore`
keeps a pointer to the caller-owned clock, which must outlive the store. Frames,
signals, events, and states own only fixed-size value data. The model performs
no heap allocation, and callers must use bounded queues/ring buffers around it
when crossing task or transport boundaries. Snapshot values are independent
copies and may be handed to a slow consumer without holding the receive path.

The unit tests cover construction, identifier/DLC validation, timestamp
ordering, valid-zero distinction, stale transitions, independent selector and
actual-gear values, per-signal freshness, semantic turn edges, and
deterministic non-mutating snapshots. No bench or vehicle validation is
claimed by MCAN-4.
