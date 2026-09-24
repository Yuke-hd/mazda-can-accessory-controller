# Generic vehicle signal consumer

`MazdaSignalProvider` exposes the fixed catalog and value-copy read/notification
contracts from `vehicle_signals`. A consumer resolves a canonical key once,
then keeps the resulting ID for reads and subscriptions:

```cpp
const auto catalog = provider.catalog();
const auto *rpm = catalog.find("vehicle.engine_rpm");
if (rpm != nullptr) {
  const auto reading = provider.read(rpm->id);
  // Check reading.status and reading.reading.availability before using value.
}

const auto *turn = catalog.find("vehicle.turn_state");
if (turn != nullptr) {
  const auto subscription = provider.subscribe(turn->id, callback, context);
  // Keep context alive until telemetry.stop() succeeds and quiesces callbacks.
}
```

The catalog view and its metadata are non-owning, immutable component data.
Keys distinguish request state (`vehicle.turn_request.left`), normalized
state (`vehicle.turn_state`), and lamp observation
(`vehicle.indicator_lamp.left`). RPM and speed are readable but do not support
notifications. Catalog metadata and descriptor-binding tests check all 18
keys against their typed state member, source CAN identifier, validation
evidence, and channel capability; host-only descriptors carry invalid IDs and
are excluded.

`mazda_signal_consumer_tests` injects generated frames through the host
acquisition source and fake clock while the production runtime, decoder,
publication store, and provider process them. It compares generic and typed
readings/notices for RPM, turn state, left request, indicator lamp, and the
front-wiper enum, including availability, validation, initial, unavailable,
and recovery flags. A `TURN_SWITCH` frame changes the request and normalized
state; a separate `BLINK_INFO` frame changes the lamp observation. This is
host parity evidence, not a claim about CAN hardware or firmware behavior.
