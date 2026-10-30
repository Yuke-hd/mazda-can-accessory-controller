# Profile runtime resource validation

This report records the R6 ESP32 retained-link measurements for the generated
vehicle-profile runtime. The probe is compile/link-only. It keeps the
production Mazda provider selection unchanged and is disabled by default.

## Build configuration

All builds used fresh out-of-tree directories and external `sdkconfig` files
with ESP-IDF `v5.5.4`, target `esp32`, and the pinned vehicle-core `0.2.1`
dependency. Production builds used `sdkconfig.defaults`. Worst-case builds also
used `sdkconfig.runtime-stats.defaults` and enabled
`CONFIG_WEACT_CAN_TELEMETRY_PROFILING=y` and
`CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y`. The runtime-stats defaults enabled
`CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS=y`.

Each row's external sdkconfig is `<build-directory>/sdkconfig`; no generated
firmware files are tracked. All six probe artifacts were built from source
commit `090d2a6f0a9dc9952dafff601446aa61145e429f`. An independent reviewer
rebuilt `service.cpp` and the probe translation unit from each probe's compile
command; all 12 objects matched the corresponding build objects byte-for-byte.
The reviewer's local hash manifest is
`/tmp/mazda-review375-final-object-proof/manifest.json`.

The probe generates C++ only below the external build directory. Its
linker-rooted anchor is never called by `app_main`. The anchor uses borrowed,
caller-aligned opaque storage for placement construction, so the retained-link
image does not contain a second `ProfileRuntimeService` object alongside the
native Mazda provider. The public service reservation and private implementation
size remain present as ELF scalar witnesses. The static notification dispatcher
pool is linked and included in the DRAM measurements.

## ESP32 size matrix

Rows are from `idf.py size` for the corresponding ELF map. The total uses the
ESP-IDF analyzer, which includes its 32-byte `.rtc.force_slow` contribution.
The DRAM totals below exclude an instantiated `ProfileRuntimeService`; that
reservation is reported separately. They include the retained runtime code,
generated profile tables, the linked dispatcher pool and all existing firmware
data.

| Build | Generated counts (messages / fields / bindings / Notify) | DRAM used / free (B) | IRAM used / free (B) | Flash code (B) | Flash data (B) | Total image (B) | External build directory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Baseline, production | no probe | 83,796 / 40,784 | 123,431 / 7,641 | 467,706 | 94,100 | 715,473 | `/tmp/mazda-r6-baseline-production-final` |
| Empty, production | 0 / 0 / 0 / 0 | 88,380 / 36,200 | 124,055 / 7,017 | 479,070 | 94,436 | 727,933 | `/tmp/mazda-r6-empty-production-borrowed` |
| Populated, production | 1 / 1 / 1 / 1 | 88,564 / 36,016 | 124,055 / 7,017 | 482,266 | 94,436 | 731,313 | `/tmp/mazda-r6-populated-production-final2` |
| Maximum, production | 64 / 64 / 64 / 17 | 98,620 / 25,960 | 124,055 / 7,017 | 498,602 | 94,436 | 757,705 | `/tmp/mazda-r6-max-production-borrowed` |
| Baseline, worst-case | no probe | 92,044 / 32,536 | 128,103 / 2,969 | 478,694 | 94,396 | 737,621 | `/tmp/mazda-r6-baseline-worst-final` |
| Empty, worst-case | 0 / 0 / 0 / 0 | 96,628 / 27,952 | 128,519 / 2,553 | 489,934 | 94,700 | 749,709 | `/tmp/mazda-r6-empty-worst-borrowed` |
| Populated, worst-case | 1 / 1 / 1 / 1 | 96,812 / 27,768 | 128,519 / 2,553 | 493,062 | 94,700 | 753,021 | `/tmp/mazda-r6-populated-worst-final4` |
| Maximum, worst-case | 64 / 64 / 64 / 17 | 106,868 / 17,712 | 128,519 / 2,553 | 510,030 | 94,700 | 780,045 | `/tmp/mazda-r6-max-worst-borrowed` |

All six probe builds produced an ELF and linker map, passed `idf.py build`, and
passed the post-link IRAM check for all 23 required hot-code symbols. No profile
was truncated. The maximum fixture fills all four provisional limits.

## Service and dispatcher storage

The linker-rooted anchor references four probe-only ELF witnesses compiled
from `sizeof`, `alignof`, and the private implementation type. It does not
allocate the service reservation in the retained-link image.

| Build mode | Private implementation (B / alignment) | Public service reservation (B / alignment) | Reservation above implementation (B) | ESP32 28 KiB ceiling headroom (B) | Dispatcher task storage + owner (B) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Production | 26,016 / 8 | 26,624 / 8 | 608 | 2,048 | 4,436 + 4 = 4,440 |
| Worst-case profiling | 26,048 / 8 | 26,624 / 8 | 576 | 2,048 | 4,448 + 4 = 4,452 |

The prior host handoff measured a 25,928-byte implementation within the
30,720-byte service reservation, leaving 2,048 bytes below the 32 KiB reference
ceiling. This R6 firmware matrix measures ESP32 link resources; it does not
measure runtime heap use or operating-system worker allocations.

The service reservation is deliberately separate from the DRAM matrix. A
separately instantiated profile service would require its 26,624-byte
reservation in addition to the linked dispatcher pool. R6 retains code and
tables without selecting a second production provider; later application
selection must account for the active provider composition.

## Linker retention evidence

- The populated map and ELF retain `vehicle_profile::detail::decode_message_0`,
  the 20-byte dispatch table, the 8-byte freshness table,
  `ProfileRuntimeServiceImplementation::process`, provider process/publish/
  dispatch, service start/stop, and
  `vehicle_core::NotificationChannel<vehicle_signals::SignalValue, 0>::dispatch_pending`.
- The maximum map and ELF retain all 64 generated decoder functions, the
  1,280-byte dispatch table, the 512-byte freshness table, provider and adapter
  paths, and notification dispatch.
- An empty profile has no decoder or dispatch/freshness array symbols because
  its generated arrays have size zero. The map and ELF instead retain the
  28-byte `kTables` wrapper in `.dram0.data`; all 28 bytes are zero. The generic
  provider, adapter lifecycle, dispatcher, and notification code remain linked.
- All six probe selections retain the anchor and the public/private service
  storage witnesses. No static service object for the anchor appears in the
  ELF.

## Validation gates

The profile generator unit tests passed (11 tests), including exact maximum
fixtures and explicit rejection of each over-limit count. Architecture checker
tests passed (42 tests), including negative cases for host-tool references and
indirect probe-source wiring. The full architecture checker and public-header
checker passed against the pinned vehicle-core `0.2.1` checkout. All six probe
images passed the linked-symbol and IRAM gates listed above.

## Limits and rejected inputs

The synthetic maximum fixture contains 64 selected messages, 64 extracted
fields, 64 bindings, and 17 Notify bindings. The focused generator test rejects
65 messages, 65 fields, 65 bindings, and 18 Notify bindings explicitly. Existing
catalog limits remain 32 enum choices, 63-byte keys, 31-byte tokens, and a
16 KiB serialized catalog. There is no silent truncation.

During implementation, a probe version with a static service object was also
measured. That artifact added a second 26,624-byte service beside the native
Mazda provider and exceeded DRAM by 712 bytes in production and 8,968 bytes in
the worst-case build. It was discarded as a double-service measurement. The
final anchor removes that second global object while retaining the same code
paths and reporting the reservation separately.

The production `app_main` provider wiring and Mazda decoder are unchanged.
Generated C++ and build products stay outside the repository. Evidence is
software-only: no image was flashed, and no bench or vehicle operation was
performed. Runtime profile selection and allocation guards remain outside R6.

## Reproduction

The build matrix used fresh build directories and an external `sdkconfig` per
row. For example, a populated production build uses:

```sh
source /path/to/esp-idf/v5.5.4/export.sh
task_repo_root="$PWD"
idf.py -C "$task_repo_root/firmware/weact-can485-v1.1" \
  -B /tmp/mazda-r6-populated-production \
  -D SDKCONFIG=/tmp/mazda-r6-populated-production/sdkconfig \
  -D SDKCONFIG_DEFAULTS="$task_repo_root/firmware/weact-can485-v1.1/sdkconfig.defaults" \
  -D WEACT_CAN_PROFILE_LINK_PROBE=populated set-target esp32
idf.py -C "$task_repo_root/firmware/weact-can485-v1.1" \
  -B /tmp/mazda-r6-populated-production \
  -D WEACT_CAN_PROFILE_LINK_PROBE=populated build
idf.py -C "$task_repo_root/firmware/weact-can485-v1.1" \
  -B /tmp/mazda-r6-populated-production \
  -D WEACT_CAN_PROFILE_LINK_PROBE=populated size
```

For worst-case profiling, set `SDKCONFIG_DEFAULTS` to the semicolon-separated
absolute paths for `sdkconfig.defaults`, `sdkconfig.runtime-stats.defaults`,
and the external defaults file containing the two profiling options listed
above. Select `empty` or `max` for the other synthetic profiles. Leave the
option at its default `off` for the no-probe baseline.
