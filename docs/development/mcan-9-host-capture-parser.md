# MCAN-9 host capture parser and replay (retired)

> Retired 2026-09-08 by S1-D / [#59](https://github.com/Yuke-hd/esp32-vehicle-can-core/issues/59).
> The custom `raw_capture` reader, writer, replay harness, capture-only tests,
> fixture, and validator were removed. SavvyCAN is the selected capture and
> replay tool; this repository does not provide a replacement parser, format,
> or adapter.

This file is retained as historical design context for the superseded MCAN-9
work. It is not an implementation guide or an active product requirement.
Requirements associated with historical #5, #8, #9, and #40, and the
canonical-exporter wording of #39, are superseded by the capture-retirement
decision in #54 and #59. Deterministic decoder and freshness tests now inject
synthetic typed frames directly through test-only helpers.
