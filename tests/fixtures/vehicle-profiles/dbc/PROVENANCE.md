# DBC import fixtures

`demo-a.dbc`, `demo-b.dbc`, `mapping-demo-a.json` and `mapping-empty.json` are
newly authored synthetic files, licensed Apache-2.0 with the rest of this
repository.

- The synthetic DBCs contain no vehicle observations, captures, VINs or real
  DBC content, and no Mazda data.
- Their message ids, signal names and scales are invented for tests only.
- Bus `body` is a logical label chosen by the mapping, not a physical bus.
- A mapping pins each source by SHA-256. Editing a DBC fixture means updating
  the digest in every mapping that names it.
- The importer's registry, profile and report are generated at test time and
  are never committed; only DBC sources and `mapping-*.json` live here.
- Import results are software evidence only, not a vehicle claim or hardware
  validation.
