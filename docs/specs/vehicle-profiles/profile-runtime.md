# Generated vehicle-profile runtime

Status: proposed module specification for owner review, issue #322. Owner
approval is required before runtime implementation tickets begin. This defines
intended behavior; current firmware still uses the Mazda provider. The
[normalized v1 contract](v1-format.md) owns host validation and identities.
Build selection and persisted configuration belong to the integration spec.

## Boundary and ownership

One build-selected profile serves one existing receive-only physical CAN input.
A portable C++17 runtime consumes immutable generated tables and reviewed native
handlers. It implements the existing vehicle_signals::SignalProvider port:
rules, replay and companion discovery use one catalog and copied readings.
They acquire no CAN identifiers, DBC records, model types or private headers.

The composition root owns provider/source lifetime. ESP-IDF, TWAI, tasks and
locks stay in private bindings. Keep companion core release 0.2.1; no generic
components are copied or modified. Telemetry producers perform no lighting
policy or LED-driver work. Public includes follow
[module boundaries](../../architecture/module-boundaries.md).

Import, recognition, exact-decimal validation and generation finish on the host.
Firmware contains checked numeric constants and reviewed handler references.
It contains no DBC parser, expression interpreter or dynamic model selection;
imported text never becomes executable code.

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
reserved enums and length mismatches invalidate affected bindings under their
explicit policy; they cannot become plausible defaults or prior valid values.

Every binding selects checked DBC extraction/conversion or a reviewed native
handler. Runtime ids address the local static catalog; canonical keys and
choice keys identify consumer meaning. Unknown ids and unsupported capabilities
return existing request errors, distinct from successful readings carrying
NoData, Stale or Unavailable.

## Native semantic handlers

A binding selects a registry entry by handler id/revision. The entry declares
allowed profiles, ordered input signature, output type/unit, invalid-value
policy and evidence eligibility. Host validation resolves aliases before arity
checks, rejects repeated logical inputs and preserves argument order.

Handlers are pure, bounded C++ functions over copied observations and explicit
prior handler state where needed. They return validity/value and bounded next
state. They do not read clocks, allocate, block, invoke consumers, access
CAN/RTOS/LED APIs or mutate another binding. Per-handler state size and maximum
work are declared and counted in the static budget; state resets on each run.

Reviewed handlers own turn-request versus lamp distinctions, combined
left/right/hazard state, selector versus actual gear, disabled flags and
reserved encodings. Preserve independent Mazda custom-handler vectors as the
baseline. Another profile selects its own reviewed policy/revision; missing
references never inherit Mazda enums, timing, confidence or exceptions.

## Publication, time and recovery

One processing owner performs extraction, handler transitions, health updates,
freshness servicing and publication. It atomically commits one coherent
generation of signal state, message health, transport health and diagnostics.
Reads copy one publication under a short private critical section and evaluate
clock-based availability after unlocking. Callbacks run after locks release.

Acquisition's local monotonic receive watermark measures transport silence.
The frame observation timestamp orders decoder observations. These remain
separate values in one boot-scoped microsecond domain; replay injects a
deterministic compatible clock. Wall-clock time is absent. Equal, older or
conflicting observations cannot refresh timestamps, overwrite newer state or
recover invalid state. Recovery requires a strictly newer valid observation
for every affected input. Transport recovery cannot revive old samples.
Multi-input handlers additionally enforce their declared coherence policy.

Expected source period is evidence, not an operational timeout. Freshness
exists only through reviewed profile/key policy. A sample stays fresh through
the inclusive deadline and becomes due afterward. Service due groups during
silence and unrelated traffic once per current timestamp. If a newer sample
follows a due transition before dispatch, publish due state before recovery
so unavailable/recovered/coalesced evidence survives.

Malformed owned messages invalidate affected bindings. Driver failure,
transport fault, stopped lifecycle and runtime failure invalidate the profile
immediately. Unknown traffic repairs neither. Startup/reset/restart expose no
old-run samples; dependent lighting stays black until eligible current samples.
Driver-failure fail-off must not await freshness expiry. Preserve bounded
retries/watchdogs and stop-failure lifetime guarantees.

Mazda brake timing remains unset pending reviewed timing evidence. Never invent
a timeout or promote it to Fresh. Only the owner-approved fresh_or_unverified
opt-in on vehicle.brake_pressed enables the factory brake action; other brake
paths require Fresh. This is an explicit Mazda policy, with no cross-profile
fallback. Confidence/evidence and operational freshness remain independent.

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

The initial implementation must enforce these ceilings at generation and
compilation. They are proposed limits for owner review, not evidence that a
generated runtime already fits the resource gate.

| Resource | Initial ceiling/contract |
| --- | --- |
| Active profiles / physical inputs | One / one |
| Selected messages / extracted fields | 64 / 64 |
| Exposed bindings / Notify bindings | 64 / 17 |
| Native handler inputs | 8 per binding, also bounded by its registry signature |
| Raw field width / payload | 64 bits / 8 bytes |
| Subscribers | Two shared typed/generic slots per Notify binding; at most 34 generated callback records |
| Enum choices / key bytes / unit/evidence token bytes | 32 per Enum / 63 / 31, counted as UTF-8 bytes |
| Serialized companion catalog | 16 KiB; overflow rejects the entire export |
| Acquisition queues | Existing 64-frame SPSC ring plus separate 64-frame driver RX queue |
| Service opaque storage | Host 32 KiB / ESP32 28 KiB; existing size/alignment guards remain authoritative |

Catalog ceilings come from SignalCatalogExport; 64 catalog entries do not
authorize 64 Notify channels. The proposed 17-channel ceiling keeps initial
callback storage at the current production Mazda channel count; accepting a
larger baseline requires fresh resource evidence and an explicit ceiling
revision. The eight-input handler cap bounds per-binding work, and individual
registry signatures may impose a smaller limit. Derive table sizes, handler state, subscriptions
and due-state storage from the selected profile. Construction, receive/decode,
poll, publication, notification and lifecycle operations allocate no heap.
Task/stack/queue reservations remain private and measured. Do not assume a
maximum profile fits: generation rejects declared limit violations, and
size/alignment/link failures block the build. Truncation or silent budget
increases cannot resolve an overflow.

Pass all effective limits to the identity stage's explicit validation_limits
mapping, so validation-bound changes affect artifact identity. They do not
change semantic compatibility.

Before implementation acceptance, measure maximum allowed profiles/handler
state in fresh production and worst-case profiling firmware. Report service
size/alignment, static DRAM, IRAM, flash, and software receive/decode/notification
performance against the same accepted baseline. Record task stack high-water
evidence when available. Enforce architecture/public-header checks, fixed
storage guards and the post-link [IRAM gate](../../development/flash-cache-layout.md).
Historical figures there and in the [facade budget](../../development/facade-storage-budget.md)
are context, not generated-runtime measurements. Exceeding a gate requires
reviewed redesign or an explicitly reduced supported ceiling.

## Independent acceptance evidence

Implementation tickets must use independent synthetic expected results:

- Intel/cross-byte Motorola, signed/unsigned 64-bit extrema, exact conversion,
  finite binary32 boundaries, invalid ranges, reserved enums and short frames.
- Every handler's valid/disabled/reserved cases, ordered arguments and reset;
  Mazda turn/brake/custom-handler regression vectors.
- Two profiles with different ids, policies and enums; separate instances
  cannot leak samples, handlers, freshness or Mazda brake eligibility.
- Coherent concurrent reads, equal/older/conflicting timestamps, malformed
  messages, transport faults, silence deadlines, newer-only recovery and
  unavailable-before-recovery dispatch.
- Capacity exhaustion, stale handles, callback mutation rejection, serial copied
  callbacks, stop timeout/retry, startup/stop/restart black and empty-profile
  all-features-disabled behavior without Mazda fallback.
- Rules/replay/BLE use the same provider without source knowledge; maximum and
  over-limit profiles exercise no-allocation/size guards and both firmware
  configurations, including missing/misplaced IRAM-symbol failures.

This specification receives independent review against Mazda contracts and
owner review before runtime coding. Tool/host/firmware results are software
evidence. Bench timing and vehicle validation remain separate future evidence;
this issue authorizes no flashing or hardware interaction.
