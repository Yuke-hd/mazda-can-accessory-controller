# Stage 3-A firmware integration

The WeAct application is the ordinary public-facade consumer. Its startup
order is:

```text
board::initialize_safe_defaults()
    -> local_argb::start()          # physical black frame and supervised worker
    -> VehicleTelemetry::start()    # facade starts strict vehicle CAN
    -> application polling cadence
```

The application registers a typed `on_turn_state_changed` callback while the
facade is stopped, then polls `speed_kph()` and `engine_rpm()` every 100 ms.
It does not call `can_bus::receive`, a decoder, `update()`, `tick()`, or
`local_argb::submit()`. CAN acquisition, decoding, freshness servicing,
notification dispatch, and LED publication remain owned by background tasks.

The service's private semantic handoff is translated by the WeAct composition
adapter through `vehicle_lighting_policy::command_for()` and
`local_argb::internal::sink()`. Only generic RGB bytes, an absolute deadline,
and an actionable bit cross the renderer boundary. Public facade headers still
contain no CAN, decoder, board, lighting, or RTOS dependency.

The ordinary application API is intentionally this small:

```cpp
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/accessory_telemetry.hpp"

mazda::VehicleTelemetry telemetry{};
auto turn = telemetry.on_turn_state_changed(&on_turn_state_changed, context);
auto speed = telemetry.speed_kph();
auto rpm = telemetry.engine_rpm();
auto started = telemetry.start();
```

The callback and polling calls consume value-only contracts. The application
does not receive frames or invoke decoder, freshness, dispatcher, publication,
or renderer operations.

The vehicle project selects `vehicle_can_rx`, `mazda_telemetry`,
`vehicle_lighting_policy`, `local_argb_sink_contract`, and `local_argb`. No
normal-mode or acknowledgement-capable CAN binding is part of this repository.

## Verification evidence

The host acceptance command is:

```sh
cmake -S . -B /private/tmp/mazda-accessory-controller-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /private/tmp/mazda-accessory-controller-host --parallel 2
ctest --test-dir /private/tmp/mazda-accessory-controller-host --output-on-failure
```

The integrated host run includes the architecture and public-header boundary
gates, receive-only artifact checks, local-ARGB boundary checks, the
facade service tests, and the policy/renderer tests. Test counts depend on
the checked-out companion-core commit; this host evidence does not claim
ESP-IDF scheduling, RMT behavior, ACK behavior, or physical vehicle
acceptance.

The pinned firmware command remains:

```sh
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
```

It requires ESP-IDF v5.5.4. If Docker or `idf.py` is unavailable, the checks
must be reported as unavailable rather than inferred from host tests. Physical
LED, CAN-load, wiring, termination, no-ACK, and warm-reset evidence is not
established by this software integration. Record the exact source and
flashed-image revisions with any isolated hardware test results.

SavvyCAN remains the selected external capture/replay tool. The retired
repository capture parser/format remains retired; the project does not add a
replacement adapter or custom parser. No active vehicle transmission or
private vehicle data is added to satisfy this integration. See
[`mcan-9-host-capture-parser.md`](mcan-9-host-capture-parser.md) for the
retirement boundary.
