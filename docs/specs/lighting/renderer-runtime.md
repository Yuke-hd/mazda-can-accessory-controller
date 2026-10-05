# Local ARGB renderer runtime

This document specifies the runtime behavior of `components/local_argb`, which
owns the fixed 100-pixel frame and the WeAct board's separate status pixel. The
action adapter's commands and held-level behavior are specified in
[local LED actions](local-led-actions.md). Board capabilities and firmware
construction are documented in [hardware capabilities](../../architecture/hardware/weact-can485-v1.1.md)
and [firmware composition](../../architecture/firmware-composition.md).

## Outputs and startup

The logical frame is 100 pixels: 35 pixels for each turn region and a 30-pixel
center brake region. Turn effects use an amber flow, the brake region uses
red, and fills use their configured color. The brightness ceiling is 16 per
channel. The one-pixel onboard status LED shows red when the center brake
region contains non-black pixels, amber when either turn region contains
non-black pixels, and black otherwise.

On the WeAct CAN485 V1.1 board, GPIO16 drives the vehicle strip through the
ESP-IDF `led_strip` SPI backend on SPI3 with DMA enabled. GPIO4 drives the
onboard status pixel through the RMT backend at 10 MHz with DMA disabled. Both
outputs use WS2812 with GRB channel order. The component pins `led_strip`
version 3.0.3.

Board initialization first drives both data pins low. `local_argb::start()`
then sends and refreshes an explicit black frame on both outputs before it
creates the worker or returns success. It retries startup black twice after the
initial attempt; if the driver cannot clear the pixels or the worker cannot be
created, startup fails and firmware must not start CAN. Holding a data pin low
alone does not clear a color latched by a WS2812B.

## Worker and queue

After startup, the `local_argb` FreeRTOS task owns ordinary frame rendering
and pixel-driver calls. Producers send a value copy through a statically
allocated, one-item overwrite queue. A new command replaces an older pending
command without waiting for rendering, and the worker also ticks every 10 ms
when the queue is idle. Identical frames are not written again. Startup black
runs on the caller before the worker exists; `fail_off()` can also write black
directly if the queue is not available.

The worker runs at `tskIDLE_PRIORITY + 2`. A separate `argb_guard` task runs at
the highest configured FreeRTOS priority and does no pixel or driver work. It
watches both the driver's active-call timestamp and a lease renewed by the
worker. A driver call or worker stall longer than 100 ms requests
`esp_restart`; the 10 ms poll gives a 110 ms request bound while the scheduler
runs. The worker lease is armed only after startup black and worker creation,
so initialization cannot trigger a false restart. Boot safe defaults and
startup black run again after a restart before CAN can start.

A failed colored frame write immediately attempts black. The renderer holds a
fault until a later command follows a successful black write; it does not try
the colored frame again on its own. If the black write fails, the worker
retries black on its next 10 ms tick. The supervisor requests a restart if a
driver call stops returning.

A controlled restart requested by another task (the companion config commit
and factory revert) goes through `fail_off_for_restart(timeout)`, never the
driver. It shuts the publish gate permanently, so later commands and resumes
are refused, waits for publishers already inside the gate (each follows a
command that raced the shut with black), then queues black and waits for the
worker to finish a pass that leaves the queue empty and black written. It
returns whether black was confirmed within the timeout; the caller restarts
either way, because startup black runs again at boot.

## Bounded transient layers

The private value-only handoff also accepts up to eight solid-zone transients.
A `LightingTransient` carries a zone, color, priority, and positive finite
`duration_us` in `1..kMaxTransientDurationUs` (five seconds). The contract
rejects zero and larger durations; longer behavior belongs to held state. An
invalid zone owns no pixels. The renderer
caps transient color channels at the same brightness ceiling as fills.

`LightingCommand::transients` is a latest-start snapshot. Each entry has a
stable renderer-local `TransientId` (1..8), nonzero sequence, monotonic trigger
time `origin_us`, and cancellation `epoch`. One authoritative publisher owns
the complete snapshot; additional sources must compose their state before
publishing. The publisher
retains each entry in subsequent held-state updates. This prevents a start
from being lost if a held update replaces it in the one-item queue. A changed
sequence starts that identity at command-application time, restarting its
full duration, only if its origin is not in the future and its age is less
than the duration, and its epoch matches the current sink generation; equal sequences do not restart. Definition changes under an
unchanged sequence are ignored: zone, color, priority and duration are copied
when the renderer accepts the start. Identities have no vehicle or action
meaning. Duplicate identities and invalid IDs/sequences are rejected by the
bounded snapshot collection without modifying it.

A transient owns only its zone and participates in the existing per-pixel
priority ordering. Higher priority wins, including dark pixels of held turn
animations. Equal priorities retain existing held drawing order, followed by
transients in snapshot order. Expiry occurs when elapsed time reaches the
duration, so an 800 ms layer applied at 100 ms ends at 900 ms. Expiry restores
the currently held result and needs no clear command. Elapsed subtraction
avoids deadline overflow; the five-second ceiling prevents a configured
one-shot from behaving as an effectively permanent held layer.

Missing identities cancel their active layers. Non-actionable commands,
expired command deadlines, backwards renderer time, startup, and driver faults
cancel transients. The renderer records seen sequences after cancellation or
expiry, so a later held update or fault recovery cannot replay them; a new
sequence is required. The sink invalidates its cancellation generation on
explicit fail-off, driver failure, and progress gate close/open. Publishers
capture that generation at trigger time and preserve it in retained starts.
This also rejects unseen starts lost behind fail-off in the overwrite queue
and starts published while the progress gate was closed. A publication racing
any cancellation-generation change (including gate close/open, explicit
fail-off, or each faulted worker pass) is followed by black and reports
publication failure, allowing the publisher to retry held state. Startup resets
this record along with held state. A transient does not extend a command's
overall actionable deadline, and generic compatibility color remains the
background when no held effects are present.

Host renderer tests cover start and zone bounds, brightness, active interval,
expiry restoring held fills, priority overlap with fills and turns, equal
priority, repeated starts, held updates through the overwrite mailbox, invalid
requests, immutable accepted definitions, fault/explicit fail-off recovery,
backwards time, stale/future origins, unseen cancellation, progress-gate
rejection and epoch-invalidation races, shared worker apply/tick fault
invalidation and explicit fail-off queue overwrites, compatibility background
restoration, and elapsed timing near the clock representation limit.

The worker receive snapshot is static storage owned solely by that task; a
compile-time guard limits the queue payload to one quarter of its 4096-byte
stack. Compiler frame measurements and this guard do not establish runtime
stack headroom for renderer, driver, logging, or publisher call chains. No
worker/publisher high-water marks have been measured on hardware; that
limitation remains an accepted, unverified risk pending bench high-water
measurement.

## Watched dispatcher progress

The composition root can register one nonblocking progress probe with
`local_argb::watch_progress()`. If the watched count does not change for two
seconds, the supervisor closes the lighting-publish gate and queues black
past that gate. The renderer commands black within two seconds plus two
supervisor polls of the last progress change; the worker writes it on its next
pass. When progress resumes, the gate reopens, but the strip remains black
until a new command arrives. The renderer does not replay held commands.

This covers a running provider whose dispatcher stops delivering notices:
without a new notice, the action engine cannot issue a `Deactivate` for its
held level. The composition root supplies the progress probe; the engine and
LED action adapter do not depend on it.

## Evidence and physical limits

Host tests in `components/local_argb/tests/rendering_tests.cpp` cover rendering,
frame suppression, and the fixed layout. The progress watchdog and publish gate
are covered in `components/local_argb/tests/progress_fail_off_tests.cpp`.
`tests/host/local_led_action_composition_tests.cpp` covers startup black,
held commands, driver failure and recovery, and dispatcher-stall fail-off. The
ESP-IDF firmware build checks the board-specific driver configuration.

No host test establishes physical LED behavior. A powered WS2812B can retain
its last color until a black frame reaches it, and a total CPU, SPI, RMT, or
LED failure can prevent that frame from being sent. The restart request is
bounded only while scheduling and the supervisor continue to run; this board
has no hardware LED power gate.

The earlier telemetry-owned deadline and heartbeat design is preserved as
historical context in [local ARGB bring-up](../../work-items/local-argb-bringup.md).
