# Generated vehicle-profile runtime

Status: owner-approved Milestone 7 scope (2026-10-29), tracked by #310 and
implemented through issues #369–#376. This defines the generated runtime
contract; current firmware still uses the Mazda provider. The
[normalized v1 contract](v1-format.md) owns host validation and identities.
Build selection, factory configuration and persistence belong to Milestone 8.

## Boundary and ownership

One compiled profile serves the existing receive-only physical CAN input. A
portable C++17 runtime consumes one immutable table of generated message
decoders. It implements the existing `vehicle_signals::SignalProvider` port:
rules, replay and companion discovery use one catalog and copied readings.
They acquire no CAN identifiers, DBC records, model types or private headers.

The composition root injects the compiled table at construction and keeps the
table and referenced data alive for the runtime's lifetime. ESP-IDF, TWAI, tasks
and locks stay in private bindings. Keep companion core release 0.2.1; no
generic components are copied or modified. Telemetry producers perform no
lighting policy or LED-driver work. Public includes follow
[module boundaries](../../architecture/module-boundaries.md).

Import, exact-decimal validation and generation finish on the host. Firmware
contains checked constants and generated decoder functions. It contains no DBC
parser, expression interpreter or dynamic model selection; imported text never
becomes executable code. Milestone 7 keeps `native:mazda` on its existing
decoder and service. It does not add a native-handler registry or ports.

## Selected traffic and extraction

The initial runtime accepts standard 11-bit identifiers (0..2047) and classic
CAN data payloads of 0..8 bytes. Extended identifiers, RTR/error frames and CAN
FD are outside the selected executable boundary. Unsupported selected constructs
fail host generation. Unexpected runtime frame shapes are rejected without
reading outside their payload; other identifiers are ignored. Preserve
listen-only TWAI, zero-length TX queue and the absence of transmission,
diagnostic polling, vehicle ACK behavior and additional physical inputs.

Generated message records declare expected payload length, occupied bits,
signedness, scale, offset and permitted range. Intel/Motorola extraction follows
the v1 bit walk. Extract up to 64 raw bits with unsigned operations and
sign-extend without signed overflow. Compare full-width raw enum values before
converting a Number to finite binary32. Unrepresentable values, invalid ranges,
reserved enums and length mismatches invalidate affected bindings; they cannot
become plausible defaults or prior valid values.

Every message uses this portable C++17 decoder function type:

```cpp
using DecodeFunction = void (*)(
    const vehicle_core::RawCanFrame& frame, DecodeResult* results,
    std::size_t result_capacity, std::size_t& result_count);
```

The runtime supplies bounded result storage and initializes `result_count` to
zero. A decoder writes no more than `result_capacity` results; each result
identifies a declared signal and carries its typed value or invalidity. A
decoder has no hidden mutable state and does not order observations, update
health or freshness, or publish readings. R1 (#370) defines `DecodeResult` and
the concrete result-bound contract. Milestone 7 generates ordinary functions
from checked normalized profiles; they may call common extraction, conversion
and enum helpers. Imported profiles use direct DBC bindings; runtime generation
rejects native-handler bindings and never omits them silently.

Each dispatch entry records the message identifier, expected payload length,
decoder function and exact output signal ids. Those ids have static lifetime
and identify the bindings affected by malformed frames or decoder faults. The
runtime checks the frame shape before dispatch, then requires one unique result
for each declared output id. Foreign, duplicate, missing or over-capacity
results invalidate the affected message outputs before the runtime commits
valid typed values through its common publication path.

The composition root injects one immutable compiled table at construction;
runtime loading and profile switching are out of scope. A small synthetic
custom function tests the extension seam, but Milestone 7 adds no production
custom handlers, mapping syntax for functions or handler registry. Cross-message
or historical state requires a future explicit interface. Runtime ids address
the local static catalog; canonical keys and choice keys identify consumer
meaning. Unknown ids and unsupported capabilities return existing request
errors, distinct from successful readings carrying NoData, Stale or Unavailable.

## Publication, time and recovery

One processing owner performs extraction, result validation, health updates,
freshness servicing and publication. It atomically commits one coherent
generation of signal state, message health, transport health and diagnostics.
Reads copy one signal publication under a short synchronization binding and
evaluate clock-based availability after leaving it. The composition root
provides the borrowed binding and fixed, non-allocating context; both outlive
the provider. Decoders, clocks and callbacks run outside this binding.

Acquisition's local monotonic receive watermark measures transport silence.
The frame observation timestamp orders decoder observations. These remain
separate values in one boot-scoped microsecond domain; replay injects a
deterministic compatible clock. Wall-clock time is absent. Equal, older or
conflicting observations cannot refresh timestamps, overwrite newer state or
recover invalid state. Recovery requires a strictly newer valid observation
for every affected input. Transport recovery cannot revive old samples.

Expected source period is not an operational timeout. For imported profiles,
an optional timeout declared in `mapping.json` is the mapping author's claim,
not reviewed timing evidence. No declared timeout means freshness remains
unverified; never infer one from DBC timing. A sample stays fresh through the
inclusive deadline and becomes due afterward. Service due groups during silence
and unrelated traffic once per current timestamp. If a newer sample follows a
due transition before dispatch, publish due state before recovery so
unavailable/recovered/coalesced evidence survives.

Malformed owned messages invalidate affected bindings. Driver failure,
transport fault, stopped lifecycle and runtime failure invalidate the profile
immediately. Unknown traffic repairs neither. Startup/reset/restart expose no
old-run samples; dependent lighting stays black until eligible current samples.
Driver-failure fail-off must not await freshness expiry. Preserve bounded
retries/watchdogs and stop-failure lifetime guarantees.

Mazda brake timing remains unset pending reviewed timing evidence. Never invent
a timeout or promote it to Fresh. Only the owner-approved `fresh_or_unverified`
opt-in on `vehicle.brake_pressed` enables the factory brake action; other brake
paths require `Fresh`. This applies to the Mazda compiled catalog only. Imported
profiles follow mapping-declared freshness, cannot inherit this exception, and
cannot express `fresh_or_unverified`. Confidence/evidence and operational
freshness remain independent.

## Lifecycle and notifications

Preserve the [provider port](../../../lib/vehicle_signals/include/vehicle_signals/signal_provider.hpp)
and [telemetry lifecycle contract](../telemetry/telemetry-contracts.md).
Configuration/subscription changes are stopped-only; the first lifecycle
mutation establishes a stable owner across stop/start cycles. Polling and
diagnostics are context-independent. Callback-originated lifecycle mutation
fails before source effects.

Typed Mazda and generic subscribers share two slots per Notify signal. Generic
adapters create no extra capacity. Handles carry channel, slot and generation;
stale handles cannot remove replacements. Callbacks run serially in one
dispatcher, copy values and preserve existing change/availability/recovery/
coalescing flags. Delivery is latest state, not a frame archive. Borrowed context
lives until successful unsubscribe or successful stop establishes quiescence.
Timed-out stop retains provider/context ownership for retry.

Evaluate the received message's group plus independently due groups.
Lifecycle/transport boundaries evaluate all groups once; repeated stable-fault
diagnostics do not repeat that global pass. Preserve deterministic bounded
dispatch and per-channel exhaustion behavior.

## Static limits and resource gates

Use the approved provisional runtime caps until measurements support a change.
They are not evidence that every permitted profile fits the resource gate.

| Resource | Initial ceiling/contract |
| --- | --- |
| Active profiles / physical inputs | One / one |
| Selected messages / extracted fields | 64 / 64 |
| Exposed bindings / Notify bindings | 64 / 17 |
| Raw field width / payload | 64 bits / 8 bytes |
| Subscribers | Two shared typed/generic slots per Notify binding; at most 34 generated callback records |
| Enum choices / key bytes / unit/evidence token bytes | 32 per Enum / 63 / 31, counted as UTF-8 bytes |
| Serialized companion catalog | 16 KiB; overflow rejects the entire export |
| Acquisition queues | Existing 64-frame SPSC ring plus separate 64-frame driver RX queue |
| Service opaque storage | Host 32 KiB / ESP32 28 KiB; existing size/alignment guards remain authoritative |

Catalog ceilings come from SignalCatalogExport; 64 catalog entries do not
authorize 64 Notify channels. The provisional 17-channel ceiling keeps callback
storage at the current production Mazda channel count; accepting a larger
baseline requires fresh resource evidence and an explicit ceiling revision.
Derive table sizes, result storage, subscriptions and due-state storage from the
selected profile. Provider and adapter construction, subscription changes,
receive/decode, reads, polling, publication, notification dispatch and stop use
fixed application storage and make no application-level C++ `operator new`
allocations. Startup is the only runtime phase allowed to reserve platform
worker resources required by the pinned runtime. On the measured AppleClang 17
host build, direct `vehicle_telemetry::Runtime` 0.2.1 startup and adapter
startup each made three C++ `operator new` calls, with no adapter delta. The
POSIX dispatcher uses `pthread_create`; pthread/libc allocator calls are outside
this counter. The reported counts measure only C++ `operator new`; overall
process heap use remains unmeasured. Task/stack/queue reservations remain
private and measured. Do not assume a maximum profile fits: generation rejects
declared limit violations, and size/alignment/link failures block the build.
Truncation or silent budget increases cannot resolve an overflow.

On ESP32, the notification dispatcher uses one shared static 4096-byte task
stack and a static task control block outside each service's opaque storage.
The pool supports one active profile service at a time; a second concurrent
start returns `InvalidState`. The pinned runtime retains its separate receive
worker resource.

Pass all effective limits to the identity stage's explicit validation_limits
mapping, so validation-bound changes affect artifact identity. They do not
change semantic compatibility.

Use focused host checks for extraction, provider behavior, lifecycle and
notifications, with independent synthetic expected values. R6 measures the
generated runtime in the retained firmware probe. Report service size/alignment,
static DRAM, IRAM and flash against the same baseline; record stack high-water
values only when an authorized hardware run supplies them. Enforce
architecture/public-header checks, fixed-storage guards and the post-link
[IRAM gate](../../development/flash-cache-layout.md). Historical figures there
and in the [facade budget](../../development/facade-storage-budget.md) are
context, not generated-runtime measurements. Defer a separate performance
campaign and extra diagnostics unless existing checks expose a problem.

## Independent acceptance evidence

Implementation tickets must use independent synthetic expected results:

- Intel/cross-byte Motorola, signed/unsigned 64-bit extrema, exact conversion,
  finite binary32 boundaries, invalid ranges, reserved enums and short frames.
- A synthetic custom function exercises the decoder extension seam.
- Keep existing Mazda decoder tests active; Milestone 8 owns comparisons
  between the existing and generated Mazda decoders.
- Two profiles with different ids, mappings and enums; separate instances
  cannot leak samples or freshness, and imported profiles cannot gain Mazda
  brake eligibility.
- Coherent concurrent reads, equal/older/conflicting timestamps, malformed
  messages, transport faults, silence deadlines, newer-only recovery and
  unavailable-before-recovery dispatch.
- Capacity exhaustion, stale handles, callback mutation rejection, serial copied
  callbacks, stop timeout/retry, startup/stop/restart black and empty-profile
  all-features-disabled behavior without Mazda fallback.
- Rules/replay/BLE use the same provider without source knowledge; maximum and
  over-limit profiles exercise no-allocation/size guards. R6 retains the
  generated functions, dispatch table, provider, adapter and notification paths
  in both firmware configurations, including missing/misplaced IRAM-symbol
  failures.

Tool, host and firmware results are software evidence. No separate performance
campaign is required in Milestone 7. Bench timing and vehicle validation remain
separate future evidence; this plan authorizes no flashing or hardware
interaction.
