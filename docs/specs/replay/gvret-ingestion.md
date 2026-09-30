# GVRET CSV ingestion

The custom `raw_capture` format, writer, replay harness, capture-only tests,
fixture, and validator remain retired. SavvyCAN/GVRET CSV is the supported
host ingestion format.

The host-only `gvret_parser` library in `lib/gvret/` accepts SavvyCAN's
native V2 CSV schema:

```text
Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

V2 `Dir` values are `Rx` and `Tx`. The parser retains that direction with the
validated frame; the replay preparation boundary admits only `Rx` rows into
the receive-only controller path. SavvyCAN terminates each data row with one
empty column after `D8`; that trailing separator is accepted, while other
unsupported columns remain errors. The legacy/project V1 schema remains
accepted for existing generated fixtures:

```text
Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

V1 rows are treated as received (`Rx`). The parser retains the source
timestamp and bus and does not open files, normalize timestamps, select a bus,
schedule replay, or couple to Mazda or firmware code. The replay preparation layer
selects the source bus, normalizes timestamps, and admits only receive rows.
The inspection CLI reports how many selected-bus `Tx` rows it skipped.

## Timestamp convention

The `Time Stamp` field is always a decimal unsigned microsecond count. SavvyCAN
may write either a capture-relative counter or a Unix-epoch microsecond value
(for example, `1790000000999900`); both forms are accepted without narrowing or
truncation. Formatted date/time text is not a GVRET timestamp, and no timezone
conversion is performed because Unix-epoch microseconds are timezone-independent.

Replay preparation treats the source value as opaque ordering data, checks the
selected receive rows for a genuine regression, and subtracts the first
selected timestamp. For example, `1790000000999900`, `1790000001000100`, and
`1790000002250000` become `0`, `200`, and `1250100` microseconds. Equal source
timestamps remain valid and retain their input order. The parser's intermediate
`ParsedGvretFrame` retains the source value for ordering, but replay preparation
rewrites `TimedCanFrame::frame.timestamp_us` before any PixelFrame, signal JSONL,
or browser-facing replay consumer sees it.

Replay preparation applies the parser result to one selected source bus and
rewrites timestamps to a zero-based relative replay clock. The host-only file
loader provides the `can-replay inspect <capture.csv> --bus <number>` command.
The command prints the selected bus, selected-frame count, relative duration,
standard and extended counts, and CAN ID bounds. It does not print payload rows
or absolute capture timestamps. A private GVRET file can be supplied at runtime
without being copied into the repository or build output.

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
