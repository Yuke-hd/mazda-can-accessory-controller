# GH-72 GVRET-to-WebSocket end-to-end tests

These tests exercise the whole web emulator chain on the host. A synthetic
GVRET CSV goes through `gvret::load_file`, the replay scheduler, the
production Mazda, action, and LED path, and the loopback WebSocket server. A
test client acting as the browser reads the result. Everything runs on
`127.0.0.1`, and no remote service is involved.

## Synthetic input

`web_emulator_tests` writes its GVRET CSV to a temporary file and loads it the
same way `gvret-led-emulator` does. The file has 16 steps, 100 ms apart. Its
timestamps start from a large absolute value, so a leak of capture time into
the stream would be visible. Each step carries:

- `0x202` engine RPM: a ramp from 0 to the red zone, a hold, then a drop back
  to a low baseline.
- `0x091` turn switch: off, then vehicle-left, vehicle-right, hazard, and off
  again.

One bus-1 row is included and must be filtered out. The file is written by
the test itself; no real capture is checked in.

## What is asserted

- **Exact frames.** The browser receives byte-identical D1 records to the
  JSONL `PixelFrameSink`. The pixel records decode strictly to the same
  `TimestampedPixelFrame`s that the C5 `TimestampedPixelFrameSink` records.
  They arrive in the same order, with replay-relative timestamps no later
  than the end of the replay.
- **Scenarios.**
  - Startup is black at t=0.
  - The RPM fill only grows during the ramp.
  - The red zone spans pixels 35–64.
  - A left turn lights pixel 65 amber, and a right turn lights pixel 34
    amber. In both cases the outside of the strip is dark.
  - Hazard lights both markers while the red zone is kept.
  - After the turn switch returns to off, the strip shows the RPM fill only.
  - At the end, the stage fails off to black and `end` arrives with the
    connection still open.
- **No source data.** Every record is a `header`, `pixels`, `playback`, or
  `end` record in the D1 schema. None has a field that could carry a CAN
  identifier, a payload, or a capture timestamp.
- **Malformed input.** The following make `load_file` fail before any
  server can start:
  - a bad header;
  - bad identifier or data hex;
  - an oversized length;
  - a timestamp regression.
- **Client disconnect.** A browser that leaves mid-replay frees the server.
  The next browser then receives the full replay from t=0.
- **Server-side replay error and shutdown.**
  - When replay validation fails inside the server, the stream closes after
    the header with no pixels and no `end`.
  - When the server shuts down mid-replay, the stream closes without `end`.
- **Renderer.** `web_emulator_renderer_js_tests` checks what the page does
  when the connection closes or errors without `end`. The strip is cleared,
  the status reports the disconnect, and every control becomes unavailable,
  so the page never looks like it is still playing.

## Out of scope

Cross-browser pixel-perfect rendering and remote deployment are not tested.
