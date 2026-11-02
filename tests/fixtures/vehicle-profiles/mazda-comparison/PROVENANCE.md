# Mazda decoder comparison fixtures

`mapping.json` pins the existing reviewed source file at
`docs/protocol/mazda_custom.dbc` by its SHA-256. The source declares version
`Mazda custom human-readable signals for SavvyCAN`. Its existing per-field
confidence and vehicle-data provenance remain authoritative in
[`signal-evidence.md`](../../../../docs/protocol/signal-evidence.md),
[`license-and-vehicle-data.md`](../../../../docs/development/license-and-vehicle-data.md),
and [`THIRD_PARTY_NOTICES.md`](../../../../THIRD_PARTY_NOTICES.md). The mapping's
required `license` note records that no separate license for the DBC is
documented and points to those existing provenance and review records. It is
not a new license grant; vehicle data is not licensed merely because it is in
the repository.
The test copies the reviewed file into a temporary directory only so the
existing importer can resolve the relative source path. No DBC copy or
generated artifact is committed.

`vectors.json` contains independently authored synthetic payloads derived
from the reviewed field coordinates and written out as bytes. The cases contain
no captured frames, absolute timestamps, personal or vehicle identifiers, or
private trip data. The numeric CAN arbitration identifiers are protocol fields,
not personal or vehicle identifiers.
The fixture and test code are project-authored Apache-2.0 material. The
per-field source confidence in `vectors.json` is carried over from the current
signal-evidence inventory. Synthetic expected values check software
consistency only and do not raise confidence or establish vehicle behaviour.

The mapping has no freshness declarations. Imported bindings therefore retain
`user_mapped` confidence with source-only eligibility, no native handlers, and
no `fresh_or_unverified` policy. The factory's native brake exception is
outside this field comparison; existing tests retain its unchanged coverage.
