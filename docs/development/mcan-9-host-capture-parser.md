# MCAN-9 host capture parser and replay

The custom `raw_capture` format, writer, replay harness, capture-only tests,
fixture, and validator remain retired. SavvyCAN/GVRET CSV is the supported
host ingestion format for the replay work tracked by epic #52.

Ticket A1 provides the host-only `gvret_parser` library in
`lib/gvret/`. It parses the expected SavvyCAN header and rows into validated
`vehicle_core::RawCanFrame` values while retaining the source timestamp and
bus. It does not open files, normalize timestamps, select a bus, schedule
replay, or couple to Mazda or firmware code. Those concerns belong to the
follow-up A2 and A3 tickets.

A2 applies the parser result to one selected source bus and rewrites timestamps
to a zero-based relative replay clock. A3 provides the host-only file loader
and `gvret-replay inspect <capture.csv> --bus <number>` command. The command
prints the selected bus, selected-frame count, relative duration, standard and
extended counts, and CAN ID bounds. It does not print payload rows or absolute
capture timestamps. A private GVRET file can be supplied at runtime without
being copied into the repository or build output.

The parser accepts synthetic CSV in host tests only. Private vehicle captures
may be supplied at runtime but must not be committed, attached to Issues or
PRs, or echoed into logs. Deterministic decoder and freshness tests continue
to inject synthetic typed frames directly through test-only helpers.

The historical MCAN-9 requirements associated with the custom format remain
retired; the current parser is an independent GVRET CSV boundary.
