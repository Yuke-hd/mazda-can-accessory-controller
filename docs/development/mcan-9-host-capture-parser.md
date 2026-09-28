# MCAN-9 host capture parser and replay

The custom `raw_capture` format, writer, replay harness, capture-only tests,
fixture, and validator remain retired. SavvyCAN/GVRET CSV is the supported
host ingestion format for the replay work tracked by epic #52.

Ticket A1 provides the host-only `gvret_parser` library in
`lib/gvret/`. SavvyCAN's native V2 CSV schema is preferred:

```text
Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

V2 `Dir` values are `Rx` and `Tx`. The parser retains that direction with the
validated frame; the replay preparation boundary admits only `Rx` rows into
the receive-only controller path. The legacy/project V1 schema remains
accepted for existing generated fixtures:

```text
Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

V1 rows are treated as received (`Rx`). The parser retains the source
timestamp and bus and does not open files, normalize timestamps, select a bus,
schedule replay, or couple to Mazda or firmware code. Those concerns belong to
the follow-up A2 and A3 tickets.

A2 applies the parser result to one selected source bus and rewrites timestamps
to a zero-based relative replay clock. A3 provides the host-only file loader
and `can-replay inspect <capture.csv> --bus <number>` command. The command
prints the selected bus, selected-frame count, relative duration, standard and
extended counts, and CAN ID bounds. It does not print payload rows or absolute
capture timestamps. A private GVRET file can be supplied at runtime without
being copied into the repository or build output.

The parser library owns only `gvret_parser`. The replay clock, acquisition
source, controller, scheduler, output stages, and `can-replay` CLI live in the
host-only `lib/replay` library under the `replay` namespace, so the parser
links no replay, controller, action, or renderer target.

The host replay scheduler advances a caller-owned monotonic clock directly;
it does not pace events against wall time. This makes event ordering and
rendered timestamps repeatable for the same normalized input and schedule.
The scheduler still drives the production `ReplayController`, whose
cross-thread publication barrier has a bounded 500 ms wall-clock
synchronization timeout. A loaded host can therefore return
`SynchronizationTimeout` even when replay time and input are unchanged; that
result is host scheduling failure, not a different vehicle observation.

The parser accepts synthetic CSV in host tests only. Private vehicle captures
may be supplied at runtime but must not be committed, attached to Issues or
PRs, or echoed into logs. Deterministic decoder and freshness tests continue
to inject synthetic typed frames directly through test-only helpers.

The historical MCAN-9 requirements associated with the custom format remain
retired; the current parser is an independent GVRET CSV boundary.
