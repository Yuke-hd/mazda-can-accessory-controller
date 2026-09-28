# MCAN-70 browser LED-strip renderer

The `gvret-led-emulator` page draws the D1 records it receives over `/ws`. It
is a pure view: every lighting decision stays in the production C++ pipeline
(`LocalArgbOutputStage` -> `JsonlPixelFrameSink`), and the browser only shows
what that pipeline emitted.

## Behaviour

- The `header` record's `pixel_count` sizes the strip; nothing assumes 100
  pixels. The renderer accepts 1 to 4096 pixels (`kMaximumPixelCount` in
  `app.js`) and treats a larger header as an invalid stream. Each `pixels` record is drawn in strip order with its exact RGB
  values, replacing the previous frame. There is no interpolation, easing,
  CSS transition, or synthesized intermediate frame. Repaints are coalesced
  to display refreshes, so a frame may be skipped but never invented.
- Region guides (first 35, center 30, final 35) are shown only when the
  header declares the 100-pixel production layout.
- The readout shows the relative replay time from `timestamp_us` and the
  connection/replay state: connecting, waiting for the header, streaming,
  complete, disconnected, or invalid.
- An `end` record marks the replay complete and keeps the final frame.
  A disconnect before `end` clears the strip and marks it with "No live
  frame". Any malformed record (bad JSON, wrong version, pixel count mismatch,
  out-of-range channel, decreasing timestamp, records out of order) makes the
  stream invalid: the strip is cleared, the reason is shown, the page closes
  the WebSocket, and later records are ignored. Unknown record types are ignored, as D1 allows.

Wall-clock pacing and the Play/Pause, Restart, and rate controls are
described in [GH-71](web-emulator-replay-clock.md). They add `playback`,
`restart`, and `rejected` records; a `restart` record is the only way a
second `header` becomes valid.

## Assets and tests

The page lives in ordinary files under `lib/replay/web/` (`index.html`,
`style.css`, `app.js`). Each asset must stay under 16,000 bytes, the portable
MSVC string-literal limit; configure fails otherwise. CMake embeds them into the executable at configure
time as raw string literals, so the emulator still serves no files from disk.
Editing an asset re-runs the configure step automatically.

- `web_emulator_assets_tests` checks the served routes and media types, the
  page structure, that `100` appears only as the production pixel count, and
  that the JavaScript and CSS carry
  no CAN identifiers, RPM or turn logic, action rules, timers, animation, or
  external URLs.
- `web_emulator_tests` streams a synthetic RPM fill, red zone, left/right
  turn, and hazard scenario through the WebSocket and requires the records to
  be byte-identical to the production JSONL sink.
- `web_emulator_renderer_js_tests` exercises the renderer state machine under
  plain Node.js with no packages. It is registered only when `node` is found,
  so Node.js is not a build requirement.
