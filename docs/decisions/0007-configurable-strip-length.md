# ADR-0007: Configure the LED strip length at runtime under a 300-pixel board maximum

Status: Accepted
Date: 2026-10-18
Retrospective: no

## Context

The LED strip length is fixed at compile time. `local_argb::kLedCount` is
`100` in `local_argb/pixel_frame.hpp`, `PixelFrame` is a
`std::array<Rgb, kLedCount>`, and the driver checks it against
`board::kVehicleLightStripPixelCount`. The factory turn and brake layout is
also fixed: 35 left turn pixels, 30 brake pixels and 35 right turn pixels,
with a `static_assert` that they add up to `kLedCount`. The configuration
validator, the renderer and the transient bindings all bound zones by the
same constant. The controller configuration spec lists the strip length as
not configurable.

The owner wants one firmware build to drive strips of different lengths,
with the length set by the user rather than by a rebuild. WLED's
recommendation of at most 512 LEDs per pin was considered. The owner chose
a maximum of 300 pixels, intended to leave a margin in stack and refresh
timing on this board. The power margin is unverified until the measurement in
the hardware record is reviewed.

A longer buffer is not free on this firmware:

- The renderer builds frames on the worker task's stack (`tick()` and
  `frame_for()` in `local_argb.cpp`), and the worker stack is 4096 bytes. A
  300-pixel frame is 900 bytes.
- `board::VehicleLightStrip::pixel_count` is a `std::uint8_t`, which cannot
  hold 300.
- WS2812 output takes about 30 µs per pixel. A 300-pixel refresh takes about
  9 ms, compared with about 3 ms today. The driver hang watchdog
  (`kDriverHangRestartUs`) is 100 ms.
- The turn animations step one pixel per phase across the turn region, so
  the region width sets the animation's cycle length.
- If the configured length shrinks, pixels past the new end are no longer
  sent. A WS2812 pixel keeps its last colour until it is written again, so
  those pixels would stay lit.

[ADR-0006](0006-generated-output-catalog.md) says board limits come from each
firmware project and are published in the output catalog as fixed limits.
The catalog is accepted but not implemented, so its format can still change
without a version bump.

## Decision

- Rename `local_argb::kLedCount` to `kMaxLedCount` and set it to `300`.
  `PixelFrame` stays a fixed array of that size. The board value
  `kVehicleLightStripPixelCount` becomes `300`, and `pixel_count` in the
  board structs becomes `std::uint16_t`.
- Add one user setting: the number of pixels on the attached strip. It lives
  in the controller configuration as an optional field, from 100 to the board
  maximum, with a default of `100`. A document without it means a 100-pixel
  strip, as today, so the schema version stays `1`.
- The setting belongs to the LED sink, not to the document root. The
  `local_argb` descriptor declares it, and the catalog publishes it, so the
  BLE service stays sink-agnostic. The persisted key is
  `sinks.local_argb.pixel_count`.
- Changing the length is a configuration commit. As
  [ADR-0002](0002-config-commit-persists-then-restarts.md) requires, the
  commit is checked in full, persisted, and followed by a restart. The
  renderer never resizes while it is running.
- Every black frame covers the board maximum, not the configured length:
  startup black, fail-off, and the black written before a restart. This keeps
  pixels past a shortened strip dark. A normal frame sends only the
  configured length.
- Frames leave the worker stack. The renderer keeps its working frames in
  static storage owned by the worker.
- The validator checks every zone against the configured length. A zone past
  the end rejects the whole document, so the commit fails and the running
  configuration is kept.
- The factory effect layout is anchored to the strip ends and keeps its
  widths: the left turn region is the first 35 pixels, the right turn region
  is the last 35, and the brake region is the middle 30 pixels. The turn
  animations keep their present timing at every length. Pixels between the
  regions on a longer strip are free for fills and transients.
- The minimum length is therefore `100`, the space the factory effects need.
  A shorter strip is out of scope until a layout for it is decided.
- The catalog publishes the setting as an integer field with its range and
  default. A zone field gives `pixels` as the board maximum and names the
  setting that bounds it at runtime. The output catalog spec owns the exact
  encoding.

This record does not claim implementation, bench, power or vehicle
validation. A strip longer than 100 pixels on this board needs a reviewed
power measurement before it is supported.

## Consequences

Easier:

- One firmware build drives any strip from 100 to 300 pixels.
- The app can offer the strip length as an ordinary catalog field.
- Short strips keep today's refresh time, because only the configured length
  is sent.

Harder:

- The validator gains a rule that relates two fields: every zone must fit
  inside the configured length. The catalog must express that bound, and an
  app must check it against the value in the same document.
- RAM grows by about 600 bytes for each frame buffer, and the static frames
  are worker-owned state that must not be shared across tasks.
- Every fail-off path must use a maximum-length black frame. A path that
  sends a configured-length black frame would leave pixels lit, so this needs
  host tests for each path.
- Released apps drop unknown keys when they write a configuration back. Until
  the iOS tolerant model (esp-can-companion#82) ships, editing a document in
  such an app resets the strip to 100 pixels. This fails safe, but surprises
  the user.
- Strips shorter than 100 pixels are not supported.
- Animation timing still depends on region width. The owner is considering a
  duration-based setting, such as a turn animation that finishes in 200 ms at
  any width, which would let the layout scale. That needs its own decision.
- The hardware record states a 100-pixel strip. It must state the 300-pixel
  maximum and the power evidence behind it.

## Alternatives

- **Keep the length at compile time and build per strip.** No runtime risk,
  but every install needs its own build, which the owner rejected.
- **A 512-pixel maximum, as WLED recommends per pin.** Rejected by the owner:
  it takes more stack and RAM, makes a full refresh about 15 ms, and raises
  the worst-case current, with no strip in hand that needs it.
- **Scale the factory layout with the strip length.** This would support
  short strips, but turn animations would run slower on longer strips.
  Rejected in favour of fixed-width regions anchored to the ends.
- **Put the length at the document root (`strip.pixel_count`).** Simpler to
  write by hand, but ties a generic document key to one sink. Rejected to keep
  sink settings with the sink that owns them.
- **Allocate the frame on the heap at the configured length.** Rejected: it
  adds allocation failure to startup, and a fixed maximum keeps black frames
  simple.

## References

- [ADR-0002: A config commit is checked in full, persisted, then restarts](0002-config-commit-persists-then-restarts.md)
- [ADR-0006: Generate an output catalog from per-sink YAML descriptors](0006-generated-output-catalog.md)
- [Output catalog spec](../specs/companion/output-catalog.md)
- [Controller configuration](../specs/configuration/controller-config.md)
- [WeAct CAN485 V1.1 hardware record](../architecture/hardware/weact-can485-v1.1.md)
