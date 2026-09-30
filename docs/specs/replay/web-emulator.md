# Local LED emulator

`gvret-led-emulator` serves the replay browser adapter only on loopback. It
loads a caller-supplied GVRET CSV, reports an ephemeral local URL, and serves
embedded HTML, CSS, and JavaScript assets:

```sh
gvret-led-emulator capture.csv
```

The default listener is `127.0.0.1`. Pass `--ipv6` (or `--bind ::1`) to use
IPv6, or `--port <number>` to choose a local port. No non-loopback bind
address is accepted, and the server has no upload endpoint or outbound network
client.

The browser connects to `/ws`. Each WebSocket session receives the same D1
messages as the C5 `PixelFrame` JSONL output: a `header`, timestamped
`pixels` records, and an `end` marker. The page draws those frames as
described in [the browser renderer](browser-renderer.md). Replay is paced
against wall time, and the page can pause, resume, restart, and change the
rate; see [replay playback](playback.md). The connection stays open
after `end` so the page can restart the replay.
Closing the process or sending SIGINT/SIGTERM stops the replay controller,
closes the active connection, and releases the loopback listener.

The command consumes capture data locally. Do not publish raw captures,
vehicle identifiers, absolute timestamps, or replay output derived from a real
vehicle. Host tests use synthetic frames only.
