# Adding a signal

How to take a new vehicle value from a CAN field to something the rule engine,
replay tools, and companion app can use. Follow the order below: evidence first,
decoder second, exposure last. Each step is one reviewable change, and a
signal can stop after any of them.

The acceleration signals are the worked example. Read these commits together
with this page:

| Step | Number signal (acceleration) | Boolean notify signal (brake) |
| --- | --- | --- |
| Evidence | `044939a` [GH-186] record 0x078 acceleration evidence | pre-existing decoder and evidence |
| Decoder | `3b95e6b` [GH-187] decode vehicle acceleration candidates | pre-existing (`decode_brake_pedal`) |
| Provider | `0efbb92` [GH-188] expose acceleration in SI units | `d3b7bba` [GH-133] expose generic brake telemetry |
| Config | built into `0efbb92` | `1decac4` [GH-133] cover owner-approved brake freshness; `fb4064b` [GH-136] add brake action to factory profile |
| Companion | `cd79a0a` [GH-189] stream signed SI acceleration | n/a |

Use the acceleration commits for a Number signal and brake for a Boolean or Enum
signal that notifies. Other config examples: `e62d38f` actual-gear action
(#154), `0d3c678` door and liftgate action (#155), `3734af0` indicator-lamp
action (#157). The foundational catalog and read work is `63c40a9` (#17) and
`3162bbd` (#18). The decoder routing change is `47e5ee9` (#220).

Ground rules (see [`AGENTS.md`](../../AGENTS.md)):

- The firmware is receive-only. A signal only ever observes frames; never add
  transmit, polling, or ACK behaviour.
- Unknown, stale, malformed, or faulted data must fail off. Do not invent a
  freshness timeout. Leave it unset until timing evidence or an explicit
  owner-approved operational default exists (acceleration got its 250 ms
  default separately in `59250a1`, #225).
- Use only synthetic test vectors. Never commit raw captures, VINs, or trip
  data ([vehicle-data policy](license-and-vehicle-data.md)).
- Synthetic tests prove the code matches the mapping, not that the mapping is
  right. They never promote confidence.

## 0. Decide the shape

Pick the signal type and capability first; they drive every later step.

| Type | Examples | Capability | Backed by |
| --- | --- | --- | --- |
| Boolean | `vehicle.brake_pressed` | Read, usually Notify | Typed notification channel |
| Enum | `vehicle.actual_gear` | Read, usually Notify | Typed notification channel |
| Number | `vehicle.engine_rpm` | Read only | Polling descriptor |

Choose a canonical key such as `vehicle.acceleration.lateral` (lowercase,
dot-separated, no vehicle make). Keys are persisted in configs and used on the
wire; renaming one later is a breaking change. Decide the unit up front, and
keep SI units where the signal is physical.

## 1. Record the evidence

Before writing a decoder, record where the field comes from.

1. **Source field.** Add the exact message ID, start bit, length, byte order,
   scale, offset, range, and value table to
   [decoder mappings](../protocol/decoder-mappings.md). If it is from
   opendbc, add the row to [opendbc provenance](../protocol/opendbc-provenance.md)
   with the pinned commit. If it is from the reviewed DBC, cite
   [`mazda_custom.dbc`](../protocol/mazda_custom.dbc).
2. **Confidence.** Assign **Reference**, **Observed**, or **Confirmed** in
   [signal evidence](../protocol/signal-evidence.md). A field with no matching
   vehicle observation is **Reference**. State what the status does not
   cover (unobserved enum values, timing, market compatibility).
3. **Open questions.** Record unknowns (axis, sign, invalid codes, period) so
   later readers do not assume them.

Do not create an evidence claim you cannot source. Update
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) if third-party
attribution changes.

## 2. Add the decoder (portable, `lib/mazda/`)

Hardware-independent decoding lives in `lib/mazda/`; it must not depend on
ESP-IDF, RTOS, or LED code.

1. **Definition** in
   [`lib/mazda/include/mazda/definitions.hpp`](../../lib/mazda/include/mazda/definitions.hpp):
   add the message ID constant, a `CandidateMessageDefinition`, and a
   `CandidateSignalDefinition` per field, with scale, offset, physical range,
   provenance, and confidence. Leave `expected_period_us` and
   `freshness_timeout_us` unset unless timing evidence exists. Candidate metadata
   stays unset even when an operational default is added in step 3.
2. **State** in [`state.hpp`](../../lib/mazda/include/mazda/state.hpp): add a
   `vehicle_core::Signal<T>` member (and a `state.cpp` update if needed).
3. **Freshness policy** in
   [`freshness.hpp`](../../lib/mazda/include/mazda/freshness.hpp): add an
   optional timeout member, unset by default. A default needs an explicit
   decision and its own change, as `kAccelerationFreshnessTimeoutUs` did in
   `59250a1`; update `signal-evidence.md`, `decoder-mappings.md` and the
   freshness column of `live-signals.md` with it.
4. **Decoder function** in
   [`mazda_candidate.cpp`](../../lib/mazda/src/mazda_candidate.cpp), declared in
   [`decoder.hpp`](../../lib/mazda/include/mazda/decoder.hpp). Follow
   `decode_acceleration` or `decode_brake_pedal`: classify the frame, reject
   wrong DLC and extended or remote frames, record malformed input without
   changing the last good value, and handle older and same-timestamp conflicting
   frames the way the neighbouring decoders do.
5. **Routing.** Register the message ID in the frame-routing `switch` and
   dispatch paths at the bottom of `mazda_candidate.cpp`, so only the owning
   decoder sees the frame.
6. **Tests** in `tests/host/mazda_candidate_tests.cpp` using synthetic payloads:
   neutral, boundary, and unrelated-bit vectors; wrong DLC; malformed then
   recovered; duplicate, older, and conflicting-timestamp frames. Add the
   vectors to the golden table in
   [decoder mappings](../protocol/decoder-mappings.md).
7. **Spec.** Update [decoder behaviour](../specs/telemetry/decoder-behaviour.md)
   if the signal changes shared behaviour.

If the field comes from the reviewed DBC, also extend the DBC comparison
([DBC metadata verification](../protocol/dbc-metadata-verification.md)).

Then run the host build and tests ([build modes](build-modes.md)).

## 3. Expose it through the provider (`components/mazda_telemetry/`)

This makes the signal visible to the rule engine and everything above it. The
steps differ by capability; `d3b7bba` (brake) and `0efbb92` (acceleration) are
the references.

**Always:**

1. **Runtime id and catalog row** in
   [`signal_catalog.hpp`](../../components/mazda_telemetry/private_include/mazda/signal_catalog.hpp):
   append a new `SignalId` (never renumber existing ones) and a `kSignalCatalog`
   row using `polled_number`, `notified_boolean`, or `notified_enum`, then bump
   `kSignalCatalogSize`. For enums, list every enumerator, including `unknown`,
   as a choice with a stable key. Validation comes from the definition's
   confidence.
2. **Unit.** If the pinned core has no matching `SignalUnit`, declare the unit
   in the catalog row, as acceleration does.
3. **Tests** in `components/mazda_telemetry/tests/`:
   `signal_catalog_tests.cpp`, `signal_provider_read_tests.cpp`, and the
   subscription and descriptor tests; plus `tests/host/signal_consumer_tests.cpp`
   and `tests/host/replay_signal_observer_tests.cpp`, which sweep the catalog
   and carry its count.
4. **Architecture doc.** Add the signal contract to
   [signal layering](../architecture/signal-layering.md) and update every place
   that states the catalog count.

**Number (Read only):** add a `PollingDescriptor` in
[`vehicle_telemetry_service.hpp`](../../components/mazda_telemetry/private_include/mazda/vehicle_telemetry_service.hpp)
(both descriptor tuples there) bound to the same `SignalId`.

**Boolean or Enum (Read + Notify)** also needs a notification channel, which
touches several files (see `d3b7bba`):

- `vehicle_telemetry_service.hpp`: a new `k...NotificationChannel` number,
  its `NotificationChannel<T, N>` alias, an entry in **both**
  `NotificationDescriptorTuple` variants (ESP and host), a `channel_` member,
  and a `subscribe_...()` declaration. Renumber the host-only test channel
  if it follows yours.
- `src/vehicle_telemetry.cpp`: the `NotificationDescriptor` row (signal id,
  name, channel, state member, message id, confidence), a
  `MAZDA_SUBSCRIBE_METHOD(...)` with the next index, and a
  `MAZDA_PUBLIC_SUBSCRIPTION_METHOD(...)`.
- `include/mazda/vehicle_telemetry.hpp`: the public `on_..._changed` callback.
- `signal_subscription_record.hpp`: bump the `notify_signal_count()`
  `static_assert`.

Channels are shared by typed and generic subscribers up to
`vehicle_core::kNotificationSubscribersPerChannel`.

The generic API must not expose CAN coordinates, Mazda state, or decoder types.
Run the [architecture checks](architecture-validation.md) when headers change.

## 4. Make it usable in configs

Rules are authored against signal keys, and the host YAML compiler validates
them against a manifest.

1. Add the key to
   [`tools/controller_signal_catalog.json`](../../tools/controller_signal_catalog.json)
   with its `type`, `capabilities`, and (for enums) `choices` matching the
   catalog row exactly.
2. Add a case to `tests/tools/compile_controller_config_test.py`.
3. Document any new rule options in the
   [controller config spec](../specs/configuration/controller-config.md).

For a worked example of exercising a new signal in a profile, see the
actual-gear, door/liftgate, and indicator-lamp examples
(`docs/specs/configuration/examples/`, `tests/host/*_actions_tests.cpp`).

A Read-only Number can be used in range and sampled-state rules. Boolean and
Enum signals with Notify can drive state and event rules. Brake-style signals
with unset freshness need an explicit, owner-approved opt-in; do not copy the
brake exception to a new signal.

## 5. Optional surfaces

Do these only when the signal should be visible there.

- **Replay and emulator.** Signal records emitted by `can-replay --signals` are
  built in `lib/replay/src/signal_record_output.cpp`; update it and
  `tests/host/replay_signal_observer_tests.cpp`, then the
  [signal observers spec](../specs/replay/signal-observers.md).
- **Companion app live signals.** Layout 2 is fixed: new slots go in
  `lib/companion_protocol/src/live_signal_layout.cpp` and need a new layout
  version, codec and stream tests, and a
  [live-signals spec](../specs/companion/live-signals.md) update. The app must
  support the new layout. The
  [extensible live-signal spec](../specs/companion/extensible-live-signals.md)
  plans catalog-driven discovery that would remove this step. Check its status
  before adding a fixed slot.
- **Lighting.** To drive an output, add a rule and action in a config profile
  (see the [lighting profile](../specs/configuration/lighting-profile.md)). The
  signal itself needs no LED code.

## Checklist

- [ ] Evidence recorded with source, confidence, and open questions
- [ ] Definition, state, freshness member, decoder, and routing added
- [ ] Synthetic vectors and failure-mode tests (DLC, malformed, ordering)
- [ ] Freshness and period left unset unless timing evidence exists
- [ ] New `SignalId` appended; catalog row, descriptor, and (if notifying) channel and public callback agree
- [ ] Count assertions and docs updated (`kSignalCatalogSize`, `notify_signal_count()`)
- [ ] `tools/controller_signal_catalog.json` matches the catalog row
- [ ] Docs updated: decoder mappings, signal evidence, signal layering
- [ ] Host build, tests, and architecture checks pass ([build modes](build-modes.md))
- [ ] Report automated results only; no hardware or vehicle claim

## Splitting the work

Prefer one small PR per step (evidence, decoder, provider, config, then
optional surfaces), as the acceleration work did. Later steps depend on
earlier ones, so stack them rather than combining them.
