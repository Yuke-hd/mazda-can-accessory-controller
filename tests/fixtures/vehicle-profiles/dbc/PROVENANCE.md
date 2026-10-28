# DBC import fixtures

## Synthetic fixtures

`demo-a.dbc`, `demo-b.dbc`, `synthetic-pack.json`, `policies.json`,
`request-demo-a.json` and `request-mazda-custom.json` are newly authored
synthetic files, licensed Apache-2.0 with the rest of this repository.

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

- Provenance is the existing record for that file: `THIRD_PARTY_NOTICES.md`
  and `docs/protocol/opendbc-provenance.md` (opendbc reference material is
  MIT-licensed from comma.ai/opendbc at the pinned commit recorded there).
- The request labels it with logical bus `body` only so it can run through the
  synthetic pack; the DBC itself declares no bus.
- The import is software evidence only. The synthetic pack binds none of its
  fields (its `rpm` rule requires Intel byte order; `EngineRPM` is Motorola),
  so the result is an explicit `all_features_disabled` profile. That is not a
  Mazda profile, a vehicle claim or hardware validation.
