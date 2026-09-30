# Historical firmware integration record

This file preserves implementation-era validation notes from the MCAN-64
firmware integration work. The current composition and startup contract is in
[`firmware-composition.md`](../architecture/firmware-composition.md).

## Historical validation evidence

The original work note recorded this host acceptance command:

```sh
cmake -S . -B /private/tmp/mazda-accessory-controller-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /private/tmp/mazda-accessory-controller-host --parallel 2
ctest --test-dir /private/tmp/mazda-accessory-controller-host --output-on-failure
```

It described the host run as covering architecture and public-header boundary
gates, receive-only artifact checks, local-ARGB boundary checks, facade
service tests, and policy/renderer tests. It explicitly did not establish
ESP-IDF scheduling, RMT behavior, ACK behavior, or physical vehicle
acceptance.

The note also recorded the firmware build invocation:

```sh
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
```

That command required ESP-IDF v5.5.4. The work note said physical LED,
CAN-load, wiring, termination, no-ACK, and warm-reset evidence had not been
established by the software integration, and advised recording source and
flashed-image revisions with isolated hardware results. These are historical
validation statements; see the current
[development build guidance](../development/firmware-build.md) and the
applicable hardware record for present procedures and evidence.

## Superseded capture-parser note

The MCAN-64 note said SavvyCAN was the selected external capture/replay tool,
the repository's retired parser/format would receive no replacement adapter,
and no custom parser would be added. That statement predates the current GVRET
ingestion specification. The durable retirement boundary for the former raw
capture format is recorded in
[`retired-raw-capture-format.md`](retired-raw-capture-format.md); current
replay ingestion behavior is specified in
[`gvret-ingestion.md`](../specs/replay/gvret-ingestion.md).
