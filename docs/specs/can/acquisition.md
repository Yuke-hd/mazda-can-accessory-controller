# CAN acquisition

This specification describes receive-task ownership, buffering, timestamps,
and statistics. The system's strict receive-only invariant and bitrate
constraints are in the
[vehicle CAN receive-only boundary](../../architecture/receive-only-boundary.md).

## Receive path and ownership

A dedicated `can_rx` task calls `twai_receive()` with a bounded poll interval.
Its priority is `configMAX_PRIORITIES - 2`, above the application
consumers. Immediately after a successful receive it obtains microseconds since
boot from `esp_timer_get_time()` and copies the bus number, timestamp,
standard/extended identifier format, RTR flag, DLC, identifier, and fixed
eight-byte payload into a `RawCanFrame` value.

The telemetry service keeps these concerns separate: after its acquisition
source returns a frame, it samples the private `MonotonicClock` seam for the
transport receive watermark and silence timeout. `RawCanFrame::timestamp_us`
remains the source observation timestamp used by decoder message and signal
ordering. Equal or older observations therefore cannot extend, clear, or
recover semantic state, while every successfully acquired frame still proves
transport liveness. Both clocks are monotonic in production; host tests use a
deterministic fake clock, and backwards readings are clamped rather than
moving a timeout watermark backwards.

The task is the only producer for a fixed-capacity, 64-frame SPSC ring. One
consumer task owns the public `receive()` calls; callers must serialize those
calls and stop that consumer before restarting acquisition. The consumer
receives a copy and never obtains storage owned by the producer. The ring
allocates no memory after static initialization. A full ring uses a
**drop-newest** policy: the arriving frame is discarded, existing FIFO order is
preserved, and both `frames_dropped` and `queue_overflows` increment. Ring push
never waits for a consumer. The notification semaphore is also statically
allocated and is given only after a successful push.

The ESP-IDF driver has a separate 64-frame RX queue. `driver_rx_missed` is the
delta of the driver's cumulative `rx_missed_count` and `rx_overrun_count`
status counters; application-ring drops are reported separately. This
distinction prevents a slow consumer from hiding a driver-level loss.

## Statistics semantics

`statistics(kSnapshot)` returns cumulative values since start or the last
reset. `statistics(kSnapshotAndReset)` returns the pre-reset interval and then
zeros receive, queued, delivered, dropped, overflow, bus-error, driver-missed,
controller-reset, and bus-off counters. It does not remove queued frames.
Queue depth remains live, and the next interval's high watermark begins at the
depth that existed at reset. The producer publishes queue depth before its
watermark, and reset reconciles one fresh depth observation after clearing the
interval counter. If reset wins the watermark exchange, it can observe the
producer's published depth; if the producer publishes after reconciliation, it
raises the watermark afterward. Consequently, a concurrent producer cannot make
the new watermark claim that an already occupied queue started empty.

`controller_resets` is one on an acquisition interval that follows a prior
successful start, and zero on the first interval. `bus_off_events` counts
unexpected `TWAI_ALERT_BUS_OFF` events. A bus-off event represents an abnormal
driver state in strict listen-only operation, but it is not itself a
controller reset. The component does not initiate active bus recovery.
`bus_errors` is the delta of the driver's cumulative `bus_error_count` status
counter. Driver loss and error counters are sampled by the receive task before
each alert poll, so they represent driver-reported counts rather than
coalesced alert occurrences.

## References

The implementation API choices were checked on 2026-08-12 against the primary
ESP-IDF v5.5.4 documentation:

- [TWAI driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/peripherals/twai.html)
- [ESP Timer](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/system/esp_timer.html)
- [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/system/freertos.html)

## Software validation evidence

Host tests exercise bitrate rejection, full frame fidelity, FIFO order,
drop-newest behavior, overflow and watermark accounting, independent
controller-reset and bus-off event counters, statistics reset, and 1,000
producer calls with an absent consumer. The structural check verifies the
public operation set, strict listen-only token and disabled TX queue in the
vehicle binding, absence of alternate modes from the vehicle component graph,
and absence of TWAI transmit calls. The WeAct artifact check additionally
verifies that no retired or acknowledgement-capable target is selected by the
vehicle project.

These checks establish software behavior and artifact structure only. The
historical MCAN-7 implementation note recorded that physical receiver, wiring,
PCB-revision, and no-ACK validation had not been performed, and associated
that work with MCAN-33. That ticket reference is historical status, not a
current issue plan. Consult the current hardware and safety evidence before
interpreting or conducting any physical validation.
