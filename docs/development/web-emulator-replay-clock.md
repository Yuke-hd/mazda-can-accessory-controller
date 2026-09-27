# GH-71 web emulator replay clock

`gvret-led-emulator` now plays a capture against wall time, and the browser
page can pause, resume, restart, and change the rate of that playback. The
server owns the replay clock; the page only sends requests and shows the
state the server acknowledges. (This is GitHub issue #71; the older
[MCAN-71 DBC metadata](mcan-71-dbc-metadata.md) note is unrelated.)

## Where the rate applies

The production pipeline still runs on replay time only. `run_replay` steps a
`ReplayClock` through CAN frames, controller polls, and renderer ticks exactly
as before, and asks an optional `ReplayGate` before each step. The emulator's
`PlaybackPacer` is that gate and is the only replay component that reads the
wall clock (`WallClock`, `SteadyWallClock` in production):

- It holds each replay-time step until its wall-time equivalent under the
  current rate (0.25x, 0.5x, 1x, 2x, or 5x). A `PlaybackTimeline` anchors
  replay time to wall time and is re-anchored at the current position on
  every pause, resume, or rate change, so no replay time is skipped or
  repeated.
- While paused it releases no step at all: no CAN frame is processed, no
  rule is polled, and no LED renderer tick runs.
- It waits in slices of at most 10 ms (`kPlaybackCommandSlice`), so commands
  and server stop requests are observed promptly even during long gaps.

Because the gate decides only *when* a step runs, never *which* step runs,
every rate and every pause pattern produces the same `pixels` sequence. The
integration tests compare the stream at each rate, and across a pause and
resume, with the production JSONL sink byte for byte.

## Protocol additions

Browser to server, as WebSocket text frames of at most 256 bytes:

```json
{"type":"control","command":"play"}
{"type":"control","command":"pause"}
{"type":"control","command":"restart"}
{"type":"control","command":"rate","rate":0.25}
```

Server to browser, in addition to the D1 `header`, `pixels`, and `end`:

- `{"type":"playback","paused":false,"rate":1}` after each header and after
  every accepted command.
- `{"type":"rejected","reason":"..."}` for a message that is not a valid
  control (`too_long`, `malformed`, `unknown_type`, `unknown_command`,
  `unsupported_rate`). Playback continues.
- `{"type":"restart"}` before a new pass. If the replay was interrupted, the
  stage first fails off and the page receives an all-black frame, as it would
  on any stop. The restart record is followed by a fresh `header` and the
  replay from t=0 with a new controller, clock, and output stage. A restart
  keeps the chosen rate and always resumes playing.

After `end` the connection now stays open so the page can restart. The
server closes it when the page closes it (close 1000), when the process
stops, or when the page sends something the emulator does not accept:
unmasked or fragmented frames (1002), binary frames (1003), or text longer
than 256 bytes (1009). Pings are answered with pongs.

## Page controls

The page adds Play/Pause, Restart, and a rate selector. They are enabled only
while a replay is streaming (Restart and rate also after `end`). The
Play/Pause label and the selected rate follow the acknowledged `playback`
record, and a `rejected` reason is appended to the status line until the
next acknowledgement. A `restart` record clears the strip and returns the
view to waiting for a header; a second header without it is still an invalid
stream.

## Tests

- `replay_scheduler_tests` covers the gate: a refused step interrupts the
  replay and fails the stage off.
- `replay_playback_tests` drives `PlaybackPacer` with a fake wall clock and a
  scripted channel: pacing at each rate, pause and resume without skipped
  replay time, restart, close, and control-message parsing.
- `web_emulator_tests` plays the synthetic RPM, red-zone, turn, and hazard
  scenario through a loopback WebSocket client: every rate yields the
  production D1 sequence and takes at least its scaled duration; nothing
  arrives while paused; restart after `end` and restart while paused replay
  the same sequence from t=0; invalid controls are rejected; protocol errors
  close with the right code. `ClientFrameDecoder` has its own unit tests.
- `web_emulator_renderer_js_tests` covers the page's playback state, control
  messages, control availability, and restart handling.

## Out of scope

Seeking to an arbitrary replay time, and reconstructing controller state from
the middle of a recording, are not supported: restart always replays from
t=0. There is no command-line option for the initial rate yet.
