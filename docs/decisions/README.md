# Architecture decision records

Each record captures one significant decision: its context, what was decided,
its consequences and the alternatives considered. Specs and architecture
documents own the current detailed behaviour; a record explains why. Use
[the ADR template](../templates/ADR.md) and the next free number. A changed
decision gets a new record that supersedes the old one, rather than an edit.

| ADR | Decision | Status |
| --- | --- | --- |
| [0001](0001-ble-companion-transport.md) | Use BLE as the companion app transport | Accepted |
| [0002](0002-config-commit-persists-then-restarts.md) | A config commit is checked in full, persisted, then restarts | Accepted |
| [0003](0003-ble-pairing-window.md) | Gate BLE pairing with Just Works and a physical-presence window | Accepted |
| [0004](0004-vehicle-io-interrupts-on-core-1.md) | Install the vehicle I/O interrupts on core 1 and leave BLE on core 0 | Proposed |
