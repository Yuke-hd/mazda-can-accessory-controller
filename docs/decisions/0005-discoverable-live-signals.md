# ADR-0005: Discover live signals through a catalog and an extensible stream

Status: Accepted
Date: 2026-10-06
Retrospective: no

## Context

The original companion live-signal layout 1 packs 19 signals into a 23-byte
frame. Current firmware uses layout 2: 21 signals in 28 bytes, adding signed
acceleration. Both use fixed protocol tables for signal offsets, Boolean
bits, enum choice codes and status positions. Adding a signal requires
another layout version and coordinated firmware/app decoding changes, even
when its type
and availability semantics are already supported.

This makes signal additions expensive to maintain. The owner wants new
readable signals to appear automatically in the companion app and explicitly
chose to keep the existing stream alongside the new one.

The generic provider already exposes semantic signal keys, types, units,
validation metadata and enum choice keys. Its runtime SignalIds and raw enum
values are implementation details, so sending those directly would couple
the app to a particular firmware build.

The solution must fit the existing BLE transport
([ADR-0001](0001-ble-companion-transport.md)) and accepted-bond access rules
([ADR-0003](0003-ble-pairing-window.md)). CAN remains receive-only, BLE must
not delay acquisition or lighting, and reported availability must not become
fresher than the provider reports. Brake freshness remains intentionally
unset.

Compatibility includes existing iOS support for fixed layouts 1 and 2. The
fixed-layout G-meter work has its own tickets and is not superseded by this
decision.

## Decision

- Add a discoverable signal catalog and a separate extensible live-signal
  notification endpoint to the companion BLE service. Preserve the legacy
  endpoint and its negotiated fixed-layout decoding path.
- Derive the export from the provider's generic Read-capable catalog for
  supported signal types. A new supported signal or enum choice must not
  require a second BLE signal table or a packet-layout version increment.
- Use semantic signal and choice keys as identities. The catalog assigns
  compact wire handles and describes their types, engineering units,
  evidence metadata and enum choices. Provider SignalIds and raw enum values
  do not reach the wire.
- Encode live values as typed, length-delimited records carrying per-signal
  availability and value presence. Version the packet grammar independently
  of catalog content. Clients can skip unfamiliar extensions without losing
  the boundaries of known records.
- Scope handle mappings to complete discovery on a connection. Rediscover
  after reconnect even if a catalog token repeats; a token is not a
  persistent schema identity. Never apply values through a partial or
  mismatched catalog.
- Let the app display discovered signals in a generic read-only list using
  catalog metadata. Keep curated dashboards through semantic-key projection
  and retain the supported legacy fallback on older devices.
- Bound discovery, buffers, record sizes, work per sampling slot and retries.
  Use fair selection when several notifications are needed to refresh the
  catalog, and maintain independent client liveness for each signal.
- Preserve the provider's availability with the existing brake-only demotion.
  Notification arrival does not establish freshness. Catalog selection is
  read-only observation with no config, storage, CAN or lighting side effect.

This accepts the architectural direction. Exact UUID assignments, binary
encodings, version advertisement, resource limits and timing rules remain
subject to the shared contract review in
[#196](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/196).
The [extensible live-signal spec](../specs/companion/extensible-live-signals.md)
owns those details. This record does not claim implementation completion or
phone, bench or vehicle validation, and does not supersede earlier ADRs or
the legacy wire contract.

## Consequences

Easier:

- Supported signal additions reuse the same exporter, packet grammar and
  generic app decoder. Newly discovered keys and enum choices can be shown
  without an app release or a new BLE slot definition.
- Firmware builds may use different runtime IDs or catalog orders without
  changing semantic interpretation after discovery.
- Older clients retain their fixed-layout path. Firmware and iOS can adopt
  the extension through separately reviewable changes against shared
  synthetic vectors.

Harder:

- Discovery adds connection setup work, paging, validation and mapping
  state. Cancellation and reconnect must discard results from an earlier
  connection; remembered handles cannot safely substitute for discovery.
- Records carry more overhead than packed Boolean bits and fixed offsets.
  At small MTUs, a complete refresh needs several notifications. Rate caps
  and fair scheduling trade refresh latency for bounded radio and host work.
- Updates can arrive for different signals in different packets. Traffic
  for one signal must not keep another signal's old Fresh display alive.
  The app needs independent expiration in addition to whole-stream stall
  detection, without inventing provider freshness timeouts.
- Keeping both streams adds service attributes, CCCD and resource costs, and
  two compatibility paths. The app must explicitly select its notifier;
  registering a byte handler alone does not control a BLE subscription.
- Generic metadata display does not provide a curated label, chart or
  vehicle-specific presentation for every new key. Such presentation remains
  optional app work, and evidence metadata is not vehicle validation.

Malformed input, unknown metadata and unsupported values must fail
conservatively. Software tests and resource accounting precede a separately
scoped physical validation session for bonding, Service Changed, restored
CCCDs, actual throughput, stack headroom and CAN/LED coexistence.

## Alternatives

- **Continue introducing a fixed layout for each signal addition.** Keeps
  packets compact and decoding simple, but repeats the firmware/app
  coordination and version-specific tables that prompted this decision.
- **Reserve spare bits or append fields to the packed layout.** Offers some
  capacity for additions, but still needs shared field assignments and
  app knowledge of new signals. Spare capacity is finite and does not
  provide discovery of type, unit, evidence or enum choices.
- **Replace the legacy stream with a single extensible layout.** Avoids
  maintaining two endpoints, but removes compatibility for existing clients.
  The owner selected coexistence instead.

## References

- [Legacy live-signal contract](../specs/companion/live-signals.md) and
  [companion BLE protocol](../specs/companion/ble-protocol.md).
- [Extensible live-signal planning spec](../specs/companion/extensible-live-signals.md).
- [Firmware tracker #195](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/195),
  [shared contract #196](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/196),
  and [iOS tracker #41](https://github.com/Yuke-hd/esp-can-companion/issues/41).
- Existing fixed-layout G-meter work:
  [firmware #189](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/189),
  [iOS #32](https://github.com/Yuke-hd/esp-can-companion/issues/32), and
  [iOS #34](https://github.com/Yuke-hd/esp-can-companion/issues/34).
- [Module boundaries](../architecture/module-boundaries.md),
  [NimBLE resource budget](../development/ble-resource-budget.md), and
  [receive-only boundary](../architecture/receive-only-boundary.md).
