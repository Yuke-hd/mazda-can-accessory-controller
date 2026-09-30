# Vehicle CAN receive-only boundary

The vehicle firmware uses a strict receive-only CAN binding. The
`weact_can485_v11_vehicle_listen_only` target installs the ESP-IDF v5.5.4 TWAI
driver with `TWAI_MODE_LISTEN_ONLY`. The mode and WeAct CAN pins are supplied
by the separately built `vehicle_can_rx` binding, not by a shared mode selector
or a public configuration value. The driver TX queue is explicitly set to
zero. The CA-IS2062A transceiver is always powered; invalid bitrates fail before
the driver is installed. There is no normal-mode or no-ACK fallback in the
vehicle target, and no normal-mode or acknowledgement-capable firmware target
exists in this repository.

The public `can_bus` header exposes only these operations:

- `start(Configuration)`;
- `stop()`;
- `receive(RawCanFrame &, timeout_ms)`;
- `statistics(StatisticsOperation)`.

It exposes no TWAI handle and no send, transmit, recovery, mode-selection, or
arbitrary-driver operation. The implementation has no `twai_transmit` or
`twai_transmit_v2` call. Host structural checks enforce this software
boundary; they do not replace a physical no-ACK measurement on an isolated,
protected test setup.

The allowed nominal bitrates are 125 kbit/s, 250 kbit/s, 500 kbit/s, and
1 Mbit/s, using the classic-CAN timing presets in ESP-IDF v5.5.4. Other rates
are rejected. The initial firmware configuration is 500 kbit/s on bus 0; this
is a bring-up setting, not evidence that a target vehicle network uses that
bitrate.

The acquisition task, bounded queues, timestamps, and counter semantics are
specified in [CAN acquisition](../specs/can/acquisition.md). Board identity,
pin capabilities, and their provenance are recorded in the
[WeAct CAN485 V1.1 hardware record](hardware/weact-can485-v1.1.md).
