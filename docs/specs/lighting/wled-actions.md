# WLED action sink

Status: specified, not implemented. [ADR-0006](../../decisions/0006-wled-espnow-action-sink.md)
records why WLED is driven over ESP-NOW. This document specifies the intended
behaviour of the adapter and the contract that its implementation issues must
meet. Nothing here is hardware- or vehicle-validated.

```text
composition root   binds ActionIds to WLED segments and wires the parts
  |
  v
ActionEngine --ActionCommand--> WledActionSink   (portable, lib/)
                                   |  latest desired state per binding
                                   v
                                WledLinkScheduler --WizmotePacket--> EspNowLink
                                   (portable, lib/)                  (ESP-IDF component)
                                                                       |
                                                        ESP-NOW unicast, fixed channel
                                                                       v
                                                    WLED: trusted MAC -> remote.json
```

The adapter knows no vehicle make, signal, rule or CAN frame. The action
engine and `vehicle_signals` know nothing of WLED; the existing architecture
checks keep `wled` tokens out of them. Only the composition root knows both.

## WLED behaviour relied on

Checked against WLED `v16.0.1` (commit `29b389df1c1a`). A WLED upgrade has to
re-check these points.

- ESP-NOW receive is enabled under Settings → WiFi, together with "Disable
  WiFi Sleep". Only senders on the trusted list (up to 10 MAC addresses) are
  processed. WLED receives on its current Wi-Fi channel: the AP channel in AP
  mode, or the channel of the network it has joined.
- A packet is accepted as a WiZmote message when it is exactly 13 bytes long
  and byte 0 is `0x80`, `0x81` or `0x91`.
- WLED drops a packet whose sequence number equals the last one it accepted.
  It does not compare older numbers. The counter resets when WLED reboots.
- The receive callback stores only the latest button. The main loop applies it
  later, so a second button arriving before then overwrites the first.
- For each button code, WLED first looks up `"<code>"` in `/remote.json`. If
  `cmd` is a JSON object, WLED applies it as a JSON-API state; built-in actions
  run only for codes the file does not define.
- The JSON state `nl` sets the nightlight. When nightlight turns from off to
  on, a timer starts. Sending `"on": true` while the timer is running does
  *not* restart it, and WLED evaluates the timer at most every 100 ms. With
  mode 0 (set), the timer's expiry sets the global brightness to `tbri`.

## Packet format

The controller sends one 13-byte WiZmote packet per command:

| Byte | Value |
| --- | --- |
| 0 | `0x81` (program) |
| 1–4 | sequence number, `uint32`, least significant byte first |
| 5 | `0x20` (button data type) |
| 6 | button code |
| 7 | `0x01` (battery data type) |
| 8 | `100` (battery level) |
| 9–12 | `0x00` |

Each new command uses the next sequence number, wrapping and skipping the
value it just used. A retry of the same send reuses its number, so WLED applies
it at most once. The first number after controller boot comes from the
injected random source: a fixed start could equal WLED's last accepted value
and be dropped.

## Bindings and code table

`WledActionSink::bind(ActionId, WledSegmentBinding)`, where
`WledSegmentBinding` is `{segment, level_steps}`:

- `segment` is a WLED segment id, 0–31. Each bound segment belongs to exactly
  one binding.
- `level_steps` is 1–8. With 1, the binding is on/off only. With *L* > 1,
  `SetLevel` selects one of *L* brightness steps.
- One action may drive several segments, through several bindings. All
  bindings are made before the engine attaches.

| Status | Cause |
| --- | --- |
| `InvalidAction` | The `ActionId` is zero. |
| `InvalidBinding` | `segment` > 31, or `level_steps` is 0 or above 8. |
| `DuplicateSegment` | Another binding already owns the segment. |
| `CapacityExceeded` | `kMaxBindings` (8) bindings exist. |
| `InvalidState` | The sink has started. |

Codes are assigned by binding index *i* (0–7), in binding order:

| Code | Meaning | `remote.json` command |
| --- | --- | --- |
| `32 + 16·i` | binding *i* off | `{"seg":[{"id":S,"on":false}]}` |
| `32 + 16·i + s`, s = 1..L | binding *i* at step *s* | `{"on":true,"seg":[{"id":S,"on":true,"bri":round(255·s/L)}]}` |
| `240` | all off (baseline) | `{"on":false}` |
| `241` | lease disarm | `{"nl":{"on":false}}` |
| `242` | lease arm | `{"nl":{"on":true,"dur":D,"mode":0,"tbri":0}}` |

Codes 32–159 and 240–242 are the only ones used. The off code leaves WLED's
global power alone. A step code sets `"on":true`, which also restores the
global brightness after an all-off or an expired lease. The generator tool
writes these exact commands from the controller configuration. The firmware
and the generated file must agree on the binding order, so both read the same
configuration. Segment geometry, colour and effect remain the owner's WLED
setup.

## Command mapping

| `ActionCommand` | Desired state of each binding of the action |
| --- | --- |
| `Activate` | Step *L* (full). |
| `Deactivate` | Off. |
| `SetLevel(x)` | x ≤ 0 or NaN: off. x ≥ 1: step *L*. Otherwise step `round(x·L)`, and off when that rounds to 0. |
| `Trigger` | Ignored in version 1. WLED cannot hold a bounded transient without its own deadline. |
| Unbound action | Ignored. |

`execute()` only writes the desired code into the binding's slot and marks it
dirty. It takes a short non-blocking lock shared with the scheduler, never
calls the link and never allocates. A new desired state replaces an unsent
one: the sink coalesces per binding and never queues history.

## Scheduler and bounds

`WledLinkScheduler::service(now)` runs on the link worker. It sends at most
one packet per call, and only when no packet is in flight and
`kMinSendSpacing` has passed since the last send completed. Candidates are
taken in this order:

1. pending all-off;
2. dirty bindings, round-robin from the last binding sent;
3. the lease step that is due;
4. a refresh of the next binding's current state, with a new sequence number.

| Bound | Value | Reason |
| --- | --- | --- |
| Bindings | 8 | Matches the local sink's binding capacity. |
| Level steps | 1–8 | Fits the 16 codes per binding. |
| Buffered state | 8 slots + 3 flags | Latest state only; no history queue. |
| In flight | 1 packet | WLED keeps only one unhandled button. |
| `kMinSendSpacing` | 50 ms | Gives WLED's loop time to apply the previous button. To be confirmed on the bench. |
| Send-completion timeout | 100 ms | A missing callback counts as a failed send. |
| Retries | 3 per packet, same sequence number | After that the link is `Unreachable`. |
| Unreachable backoff | 1 s between probes | The probe resends the highest-priority pending packet. |
| Refresh period | each binding at least every 2 s | Repairs lost or overwritten buttons and a WLED reboot. |
| Lease | disarm, ≥ 200 ms gap, arm, every 20 s; `D` = 1 min | WLED blacks out within about 1 min of silence. |

At most 20 packets per second leave the controller, and steady state is about
4 packets per second for 8 bindings plus the lease.

The scheduler reports `Idle`, `Connected`, `Retrying` or `Unreachable` and
counts sent packets, failures, retries and unreachable entries for logging.
These counters are diagnostics only and never gate CAN or local lighting.

## Lifecycle and failure policy

- **Startup.** The composition root binds the sink and registers it with the
  engine before `attach()`, as with the local sink. Commands that arrive before
  the link starts only update slots. After vehicle CAN starts, the root starts
  the WLED link asynchronously, like `companion_ble::start()`. The link sends
  all-off first, then the lease arm, then every binding's current state. A
  radio or ESP-NOW initialization failure is logged and leaves WLED unused.
  It never stops CAN, telemetry, local lighting or fail-off.
- **Unavailable WLED or radio.** When sends fail, the scheduler retries, then
  backs off. Desired state keeps coalescing. On the first successful send it
  sends all-off, the lease arm and every binding, because WLED may have
  rebooted into an unknown state.
- **Fail-off.** `fail_off()` sets every binding to off and queues all-off. The
  composition root calls it wherever it calls `local_argb::fail_off()`: after a
  provider stop or engine detach, and on a dispatcher progress stall, with the
  same bound and resume rules as the local renderer. A later engine command is
  needed before anything lights again. Before a controlled restart, the root
  makes one best-effort all-off send of at most 100 ms, then restarts anyway.
- **Controller silence.** Power loss, a crash or a dead radio leaves WLED
  holding its last state until the lease expires. Then WLED's global
  brightness becomes 0, within one minute plus WLED's 100 ms nightlight tick.
  That is the WLED-side fail-off bound. It is coarse by design (see
  ADR-0006).
- **Stop.** If the link is ever stopped, it sends all-off, then the lease
  disarm, best effort and bounded, then shuts down ESP-NOW and the radio.

## Configuration

A later configuration change adds an optional `wled` section to the
controller configuration:

- the peer MAC address;
- the Wi-Fi channel, 1–13;
- the bindings, each `{action, segment, level_steps}`.

The section is absent by default, and without it no radio starts. The
configuration validator rejects a section that the sink would reject. Example
files use locally administered placeholder addresses such as
`02:00:00:00:00:01`, never a real device MAC.

## Test seam and host verification

The portable code, with `WledActionSink`, `WledLinkScheduler`, the packet
encoder and the code table, depends only on `action_engine` and
`vehicle_core`. It reaches the radio through:

```cpp
class EspNowLink {
public:
  // Starts one send. Returns false if the link cannot accept it now.
  virtual bool send(const WizmotePacket &packet) noexcept = 0;
  // Completion of the last accepted send: delivered, failed or still pending.
  virtual SendResult poll_completion() noexcept = 0;
};
```

The scheduler also takes a `vehicle_core::MonotonicClock` and a random source
for the first sequence number. The ESP-IDF component implements the link with
`esp_now_send()` and its send callback, and owns all Wi-Fi and ESP-NOW calls.
A host `FakeEspNowLink` records packets and scripts delivery results.

Minimum host cases:

- Packet bytes for a known sequence number and code. Wrap-around skips the
  previous value. A retry reuses the sequence number.
- Code table and generated `remote.json` agree for 1 and 8 bindings and every
  `level_steps`.
- Every `bind()` status, and binding after start.
- Command mapping, including `SetLevel` at 0, a small positive value, step
  boundaries, 1, +∞ and NaN; `Trigger` and unbound actions send nothing.
- Coalescing: several commands between sends produce one packet with the
  latest state.
- Spacing, a single packet in flight, the completion timeout, three retries,
  `Unreachable`, backoff, and the full resync after recovery.
- Startup order (all-off, lease arm, bindings). `fail_off()` jumps the queue.
  No binding re-lights without a newer command.
- Lease cadence: disarm, then arm no sooner than 200 ms later, every 20 s.
  Refresh of every binding within 2 s.
- A composition test: fake provider → engine → `WledActionSink` → fake link,
  showing on, level and fail-off on `Stale` data.
- Architecture checks: the new portable target links only `action_engine` and
  `vehicle_core`. No ESP-IDF, Wi-Fi or ESP-NOW header appears outside the
  firmware component. `action_engine` and `vehicle_signals` still contain no
  WLED token.

Bench checks, reported separately from host tests:

- the flash, heap and core-0 budget with NimBLE and coexistence enabled;
- BLE companion behaviour while ESP-NOW runs;
- `kMinSendSpacing` against a real WLED device;
- lease expiry after the controller loses power;
- CAN counters (`rx_missed`, `rx_overrun`) unchanged under ESP-NOW load.

## Implementation backlog

| Issue | Work | Depends on |
| --- | --- | --- |
| [#245](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/245) | Measure the Wi-Fi and ESP-NOW budget beside NimBLE (bench spike). | — |
| [#246](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/246) | Portable sink, scheduler, packet encoder and host tests. | — |
| [#247](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/247) | Controller-config `wled` section and `remote.json` generator. | #246 |
| [#248](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/248) | ESP-NOW link component and firmware composition. | #245, #246, #247 |

A WLED usermod with a sub-second lease is deliberately not planned; see
ADR-0006.
