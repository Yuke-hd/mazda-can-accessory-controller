# DBC import fixtures

## Synthetic fixtures

`demo-a.dbc`, `demo-b.dbc`, `synthetic-pack.json`, `policies.json` and
`request-demo-a.json` are newly authored synthetic files, licensed Apache-2.0
with the rest of this repository.

- The synthetic DBCs contain no vehicle observations, captures, VINs or real
  DBC content.
- Their message ids, signal names and scales are invented for tests only.
- Bus `body` is a logical label chosen by the request, not a physical bus.
- Evidence `synthetic/demo-review` is a test reference, not a vehicle claim.

## Repository DBC baseline (owner decision)

The owner chose the existing repository file `docs/protocol/mazda_custom.dbc`
as the walking-skeleton baseline input, overriding the spec's synthetic-only
fixture rule for this one case. It is referenced in place by
`request-mazda-custom.json` (path relative to the repository root); no copy
of it exists under this directory, and no Mazda recognition pack is added.
`request-mazda-custom.json` is therefore not a synthetic fixture.

- The file is the repository's capture-derived DBC. It is governed by its own
  provenance and vehicle-data review, per `THIRD_PARTY_NOTICES.md`. Its license
  classification is pending the owner decision recorded in #358.
- `MazdaBaselineTests` hard-codes the file's current message count, field count
  and the `EngineRPM` byte order. Any edit to the DBC breaks those tests on
  purpose, so update them together with the file.
- The request labels it with logical bus `body` only so it can run through the
  synthetic pack; the DBC itself declares no bus.
- The import is software evidence only. The synthetic pack binds none of its
  fields (its `rpm` rule requires Intel byte order; `EngineRPM` is Motorola),
  so the result is an explicit `all_features_disabled` profile. That is not a
  Mazda profile, a vehicle claim or hardware validation.
