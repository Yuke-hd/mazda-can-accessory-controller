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
