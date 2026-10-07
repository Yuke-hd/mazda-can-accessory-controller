# Telemetry contracts

This document describes the current value and lifecycle contracts exposed by
the Mazda telemetry façade. Generic value-copy contracts come from the pinned
companion core. For header ownership and dependency direction, see
[Module boundaries](../../architecture/module-boundaries.md); host validation
procedures are in [Architecture and boundary validation](../../development/architecture-validation.md).

The application-facing `VehicleTelemetry` façade is non-copyable and provides
fixed polling and notification channels. Its public contracts carry copied
values; decoder/service handoffs remain internal. The WeAct application uses
the façade without manually receiving frames, decoding signals, managing
freshness, dispatching notifications, or driving the LED. See
[Firmware composition](../../architecture/firmware-composition.md).

## Clock domain

The ESP default `mazda::internal::SteadyClock` reads
`esp_timer_get_time()` directly. CAN acquisition timestamps and the WeAct
application clock use that same ESP-IDF boot-scoped monotonic microsecond
domain, so freshness ages and transport watermarks compare compatible values.
Host composition continues to inject any `vehicle_core::MonotonicClock`, which
keeps deterministic tests independent of the host's system clock. Freshness
timeouts, inclusive freshness boundaries, source timestamp ordering, and
fail-off policy do not depend on the clock implementation.

## Notification evaluation

The service keeps one processing owner for observation, publication, and
freshness servicing. A received frame evaluates only notification descriptors
whose message identifier owns that frame. Descriptors with the same identifier
form one group, so a gear frame evaluates both gear signals, a door frame
evaluates all six door signals, and the other groups follow the same catalog
ownership. Polling-only signals and unknown identifiers do not evaluate a
notification group.

Freshness is serviced independently of frame traffic. A signal remains fresh at
its configured deadline and becomes due only after that boundary; each due
signal is serviced once for its current observation timestamp. All due groups
are serviced together when a processing opportunity occurs, including silence
and unrelated traffic. If a newer observation arrives after a due transition
but before dispatch, the service publishes the due state first and then the
newer state so the notification channel can retain unavailable, recovered,
became-unavailable, and coalesced evidence. Equal, older, or conflicting
observations do not refresh a signal's timestamp or recover it. Brake freshness
remains unset unless a caller explicitly supplies a timeout.

Lifecycle and transport boundaries evaluate every notification group once so
fail-off and recovery reach unaffected subscribers. An owned message-health
boundary evaluates its affected group plus any independently due groups.
Repeated diagnostics in a stable failure do not repeat the global pass; private
lighting heartbeat publication remains independent of notification selection.

## Subscriptions and lifecycle

Each notification channel has two fixed subscriber slots. A subscription
handle identifies its channel, slot, and generation, so an old handle cannot
remove a later registration that reused its slot. Configuration and
subscription changes are stopped-only operations.

The host thread or ESP-IDF task that performs the first lifecycle mutation
(configuration, subscription, sink binding, start, or stop) becomes the
lifecycle owner and must perform later lifecycle mutations, including stop
and start cycles. Polling and diagnostics are context-independent. The owner
is represented by a bounded, allocation-free 64-bit generation token rather
than a thread-local address or FreeRTOS task handle, so sequential host threads
and recycled ESP-IDF task handles cannot inherit an earlier owner's identity.

Callbacks receive copied values and borrow their caller-provided context until
`stop()` returns successfully. A callback-originated `configure`, `start`,
`stop`, `subscribe`, or `unsubscribe` call is rejected before it changes
lifecycle state or causes source side effects. A successful stop establishes
callback and worker quiescence. A timeout or other stop failure leaves the
facade and callback context live so the lifecycle owner can retry; do not
destroy the facade or release callback context until that retry succeeds.
Runtime task and driver ownership remains private to the background service.
