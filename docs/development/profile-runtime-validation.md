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
commit `090d2a6f0a9dc9952dafff601446aa61145e429f`. At that source, an independent
reviewer rebuilt `service.cpp` and the probe translation unit from each probe's
compile command; all 12 full objects matched byte-for-byte. The later
formatting-only source commit `48ab7ad7391790dfeb7a4063ab06473578b83e1c` changes
full object bytes because debug metadata reflects the reformatted source, but
independent recompilation confirmed that every allocated section's bytes,
flags, and alignment, every relocation targeting allocated sections, and every
allocated symbol definition match the measured objects. The linked runtime
content and measurements therefore remain unchanged. The reviewer's local
manifests are `/tmp/mazda-review375-final-object-proof/manifest.json` and
`/tmp/mazda-review375-format-proof/manifest.json`.

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
  `vehicle_core::NotificationChannel<vehicle_signals::SignalValue, 0>::dispatch_pending`
  (renamed `signal_notification::NotificationChannel` since GH-390; see below).
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

## R7 runtime consumer acceptance

The R7 host test generates two populated profile tables and one empty table
from the profile projection code into the build directory. The populated
fixtures use distinct profile keys, runtime ids, CAN identifiers, numeric
scales, freshness declarations and enum maps. Tests feed the generated frames
through the provider and existing ActionEngine, replay observer fan-out,
catalog export and extensible live-signal sampler. Assertions inspect decoded
signal values and every live packet record's handle, type, status and value.
The empty profile emits an empty catalog and the exact header-only live packet;
it has no Mazda fallback.

The declared 500 ms timeout remains Fresh at its inclusive deadline and becomes
Stale one microsecond later. The declared number is actionable under Fresh at
the inclusive deadline and deactivates after expiry. Signals with undeclared
freshness remain unverified and fail off under Fresh. The generated imported
profiles use only Fresh action rules; no Mazda freshness exception or decoder
comparison is used. Future profile application/config selection must preserve
the import contract and never confer the Mazda brake opt-in on imported signals.

The allocation guard instruments C++ `operator new` and `operator new[]`.
Against the exact pinned vehicle-core `0.2.1` source at
`b3d87ac094576f7a787114a1bd24db5cf427a4f7`, the measured AppleClang 17 run
observed three calls for direct `vehicle_telemetry::Runtime` startup and three
for populated adapter startup, for a zero-call delta. Provider and service
construction, subscription changes, frame ingestion and dispatch, reads,
polling and stop each recorded zero calls. The guard does not intercept
pthread/libc allocations or establish whole-process heap use. Startup's
platform worker reservation is the approved exception; no application-level
storage is added for steady operations.

The fresh integrated host build passed all 854 CTest cases, including the
existing Mazda suite, architecture and public-header boundary gates, and the
R7 generated consumer tests. The complete `tests/tools` Python suite ran 591
tests: 590 passed and one was skipped. Validation commands:

The 854-case full run used integrated head `a72053a`. R6 then made formatting-
only source changes in `48ab7ad` and documentation-only changes in final head
`b966841`. The R7 target was rebuilt on `b966841` and passed both focused
cases; the Python suite also reran after the formatting update with the same
591-test result. The full CTest suite was not repeated for those formatting and
documentation-only changes.

```sh
cmake -S . -B /tmp/mazda-m7-r7-integrated-host \
  -DVEHICLE_CAN_CORE_SOURCE_DIR=/tmp/mazda-m7-host-gh372/_deps/vehicle_can_core-src
cmake --build /tmp/mazda-m7-r7-integrated-host -j4
ctest --test-dir /tmp/mazda-m7-r7-integrated-host --output-on-failure
MAZDA_VEHICLE_CAN_CORE_ROOT=/tmp/mazda-m7-host-gh372/_deps/vehicle_can_core-src \
  python3 -B -m unittest discover -s tests/tools -p '*_test.py' -v
```

This is software evidence only. The R7 work was not flashed, and no bench or
vehicle operation was performed. The C++ allocation counter does not measure
pthread/libc allocator activity.

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

## GH-390 notification ownership and core 0.3.0

GH-390 moved the notification contract and channel into the repository-owned
`signal_notification` library and bumped the pinned vehicle core to `0.3.0`
(`e9db379fc0f0edffa39f54b5133c1b2ac24f5fc1`). The R6 rows above stay as
historical `0.2.1` measurements. The rebuilt images below used fresh
out-of-tree directories, ESP-IDF `v5.5.4`, target `esp32`, and a
`dependencies.lock` re-resolved by `idf.py update-dependencies` to `0.3.0`.
The worst-case row used the same defaults as the R6 worst-case row; the
populated row used `WEACT_CAN_PROFILE_LINK_PROBE=populated`.

| Build | Flash code | Flash data | IRAM used / free | DRAM used / free | Total image |
| --- | ---: | ---: | ---: | ---: | ---: |
| Production | 467,806 | 94,116 | 123,431 / 7,641 | 83,796 / 40,784 | 715,589 |
| Worst-case | 478,734 | 94,412 | 128,115 / 2,957 | 92,044 / 32,536 | 737,689 |
| Populated production | 482,370 | 94,452 | 124,055 / 7,017 | 88,564 / 36,016 | 731,433 |

DRAM is unchanged from the R6 rows for all three builds. All three images passed
the IRAM placement gate with 23 `hot_code.lf` symbols in `.iram0.text`. The
populated map and ELF retain
`signal_notification::NotificationChannel<vehicle_signals::SignalValue, 0>::dispatch_pending`,
and no image links a `vehicle_core` notification symbol. The empty and maximum
probes were not rebuilt for GH-390.

The fresh UBSan host build passed all 855 CTest cases, including
`signal_notification_channel_tests`, `architecture_contracts`, and the public
header boundary gate. The `tests/tools` Python suite ran 592
tests: 591 passed and one was skipped. Evidence is
software-only: no image was flashed and no bench or vehicle operation was
performed.

## GH-398 selected application composition and resource matrix

GH-398 measures the selected service in the real firmware application. Unlike
the historical retained-link probes above, each generated-profile row builds
the application with one `ProfileRuntimeService` and its selected tables.
Native rows keep one Mazda `VehicleTelemetry`; no second service is linked.
All eight images were freshly built after #306 merged at `31e22a9`, from
post-merge integration source `8267cf0`, with ESP-IDF `v5.5.4`, target `esp32`,
pinned vehicle-core `0.3.0`, separate out-of-tree build directories and an
external `sdkconfig` per row under `/tmp/mazda-m8/builds/398-post306/`. The
final formatter-only source fix in `31e4a26` changes indentation only in
`application.hpp`: the file remains 70 lines and the diff is empty when
whitespace is ignored. All eight existing build directories were incrementally
rebuilt and re-sized on exact final source `31e4a26`; the values below are the
final-head results. The final manifest is
`/tmp/mazda-m8/builds/398-post306/final-31e4a26-manifest.json`; each row
contains its sdkconfig, ELF, BIN and linker-map SHA-256 values and build/size
logs named `build-final-31e4a26.log` and `size-final-31e4a26.log`.

The populated profile maps two independently authored synthetic DBC sources
into one message with three extracted fields and one signal binding. The empty
profile has one source and message but no signal binding. The maximum profile
uses the existing 64-message, 64-field, 64-binding, 17-Notify synthetic limit
fixture. It is selected as the application's actual generated profile and
service by the explicit resource-only
`WEACT_CAN_PROFILE_RESOURCE_FIXTURE=max` configure option. That option requires
the normal `VEHICLE_PROFILE=native:mazda` setting, is off by default, and prints
a diagnostic that the maximum fixture is not a vehicle-profile selection.
It cannot be combined with the retained-link probe. The generator identifies
the synthetic profile and checks that its artifact identity matches the
capability report.

Profiling rows enabled
`CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS=y`,
`CONFIG_WEACT_CAN_TELEMETRY_PROFILING=y`, and
`CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y`. Native rows include the existing Mazda
profiling and freshness loggers. Imported-profile rows omit those Mazda-only
loggers; the FreeRTOS task statistics logger remains active without telemetry
pause counters. Each generated-profile live engine and scratch configuration
check uses `UnverifiedFreshnessPolicy::Reject`; the native Mazda constructors
retain their existing policy.

| Application selection | Profile counts (sources; messages / fields / bindings / Notify) | DRAM used / free (B) | IRAM used / free (B) | Flash code (B) | Flash data (B) | Total image (B) | App binary |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Native Mazda, production | native | 83,996 / 40,584 | 123,431 / 7,641 | 469,342 | 94,024 | 717,217 | `0xaf210` |
| Native Mazda, profiling | native | 92,236 / 32,344 | 128,115 / 2,957 | 480,282 | 94,320 | 739,321 | `0xb4870` |
| Two-source imported, production | 2; 1 / 3 / 1 / 0 | 83,820 / 40,760 | 108,827 / 22,245 | 431,214 | 92,456 | 660,341 | `0xa13f0` |
| Two-source imported, profiling | 2; 1 / 3 / 1 / 0 | 89,468 / 35,112 | 110,979 / 20,093 | 433,314 | 92,736 | 668,529 | `0xa33e0` |
| Empty imported, production | 1; 1 / 2 / 0 / 0 | 83,652 / 40,928 | 108,827 / 22,245 | 427,686 | 92,456 | 656,645 | `0xa0580` |
| Empty imported, profiling | 1; 1 / 2 / 0 / 0 | 89,308 / 35,272 | 110,979 / 20,093 | 429,806 | 92,736 | 664,861 | `0xa2590` |
| Maximum imported, production fixture | 1; 64 / 64 / 64 / 17 | 93,900 / 30,680 | 108,827 / 22,245 | 447,338 | 92,456 | 686,545 | `0xa7a40` |
| Maximum imported, profiling fixture | 1; 64 / 64 / 64 / 17 | 99,548 / 25,032 | 110,979 / 20,093 | 449,478 | 92,736 | 694,773 | `0xa9a70` |

ESP-IDF size-report values include its 32-byte `.rtc.force_slow` contribution
in the total image size. All eight application binaries passed the existing
smallest app partition check (`0x1e0000` bytes). The native images passed the existing
post-link IRAM gate for all 23 `hot_code.lf` functions. Imported images omit
that Mazda-only fragment because they do not contain those native handlers;
their used/free IRAM is reported above. The profile service storage guard
remains enabled.

On final source `31e4a26`, each native ELF retains one application `telemetry`
object of `0x7000` (28,672 bytes), the Mazda telemetry service and no
`ProfileRuntimeService`. Each imported ELF retains one `telemetry` object of
`0x6800` (26,624 bytes), one selected `ProfileRuntimeService` process path,
and no Mazda telemetry service. Its dispatcher pool is `0x1154` bytes in
production and `0x1160` bytes in profiling, plus a 4-byte owner. The populated
profile retains one decoder function, a `0x14`-byte dispatch table and an
`0x8`-byte freshness table. The empty profile has no decoder or dispatch or
freshness table symbols. The maximum fixture retains all 64 generated decoder
functions, a `0x500`-byte dispatch table and a `0x200`-byte freshness table.
No application ELF contains the old `vehicle_profile_link_probe_anchor`.

The normal imported-profile selection remains `VEHICLE_PROFILE=<mapping.json>`.
The two-source and empty rows use the synthetic mappings under
`tests/fixtures/vehicle-profiles/dbc/`; the maximum fixture mode is only for
resource measurements. The post-#306 finalboot integration keeps repair upload
available after a failed configuration start, while vehicle CAN remains
stopped and lighting remains black. Once a normal run starts, every application
selection uses `TWAI_MODE_LISTEN_ONLY` with a zero-length transmit queue. No
image was flashed, and no bench or vehicle operation was performed.

## GH-401 integrated acceptance (pre-#306 partial record)

This historical section records partial checks before #306 merged and before
the finalboot repair path was integrated. The final post-#306 evidence below
supersedes its blocker and partial-acceptance statements. The earlier full
fresh host build and CTest run passed on `0fad8a9`: all 864 tests passed in
325.37 seconds, including `selected_profile_config_matrix`, architecture and
public-header contracts, the Mazda suite, and the runtime consumer tests. The
Python tooling suite on that head ran 618 tests: 617 passed and one was skipped
in 157.56 seconds. The exact unittest summary is preserved at
`/tmp/mazda-m8/builds/401-final-host/python-tooling-summary.log`; this is a
summary file, not a full stdout log.

The subsequent test-only CMake change at `e095cf3` fixes the matrix runner's
multi-configuration executable path. On that head, a fresh `Ninja Multi-Config`
configure followed by
`ctest -C Debug -R '^selected_profile_config_matrix$' --output-on-failure`
passed 1/1 in 27.05 seconds. The full host suite was not repeated after this
test-runner-only change. On `e095cf3`, all PR #412 host, firmware, and Host ASan
+ UBSan CI checks passed in Actions run `41440486821`.

At the time of this partial record, final saved-override boot fallback still
depended on PR #306. PR #306 has since merged at `31e22a9`; the eight-image
matrix above was refreshed on the post-merge source and the repair path was
revalidated on the finalboot integration head. See the final acceptance record
below. These results remain software evidence only; no bench, vehicle, flash,
or hardware validation was performed.

## GH-401 final post-#306 acceptance

This record supersedes the pre-#306 partial status above. PR #306 merged at
`31e22a9`. The finalboot integration at `8267cf0` keeps the companion repair
path available after configuration-start failure while leaving vehicle CAN
stopped and the lighting output black. The final source `31e4a26` differs from
`8267cf0` only by indentation in `application.hpp`: both versions have 70 lines,
and `git diff --ignore-all-space` reports no source change.

A fresh host configure and build on `8267cf0` used the supported external
build path `/tmp/mazda-m8/builds/401-post306-host`, with Ninja, Debug,
`BUILD_TESTING=ON` and `MAZDA_BUILD_HOST_TESTS=ON`:

```sh
cmake -S . -B /tmp/mazda-m8/builds/401-post306-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-m8/builds/401-post306-host --parallel 2
ctest --test-dir /tmp/mazda-m8/builds/401-post306-host --output-on-failure --parallel 4
python3 -m unittest discover -s tests/tools -p '*_test.py'
```

The full CTest run passed 869/869 in 245.72 seconds, including architecture,
public-header and selected-profile matrix checks. The Python tooling suite ran
622 tests in 161.029 seconds: 621 passed and one was skipped. Full host logs are
under `/tmp/mazda-m8/builds/401-post306-host/logs/`; the complete Python output
is `/tmp/mazda-m8/401-post306/logs/python-tooling.log`. The first CTest attempt
placed its build directory under the worktree, so the nested profile matrix
rejected that output path. I discarded that run and repeated configure, build
and all CTest cases with the external path above; no source change was made for
the path mistake.

The follow-up source change at `31e4a26` is whitespace-only. An incremental
host build passed, the formatter check passed for the changed header, and a
focused CTest run passed 4/4: `selected_profile_config_matrix`, both override
apply failure cases, and `companion_ble_startup_contracts`. The registered
selected-profile matrix CTest passed across its A/B/empty selections. The
nested CTest log directory is reused, so its `LastTest.log` records the final
empty selection only: 10 passing cases, including refused startup, later
startup refusal and empty-provider behavior. The outer CTest log records the
matrix pass. Logs are
`/tmp/mazda-m8/builds/401-post306-host/logs/ctest.log` and
`/tmp/mazda-m8/builds/401-post306-host/selected-profile-config-matrix/Testing/Temporary/LastTest.log`.

The final eight-image matrix above was freshly built after #306 merged and
incrementally rebuilt and sized again on `31e4a26`. All eight app binaries fit
the `0x1e0000` partition; native rows pass the 23-symbol IRAM gate. The report
records the selected service, storage, table, decoder, sdkconfig, ELF, BIN and
linker-map evidence in the final manifest linked above. The production
sdkconfig SHA-256 is
`213962419c4c159e169d08f7e22c219965354d7575631f97fd07bde4abeb646b`; profiling
sdkconfigs use `ef4a5761fd1a5dc89dba011fbec6c0b82eb3310f56799f32ec6725a77e608be8`.
Profiling enables FreeRTOS runtime stats, telemetry profiling and freshness
debug; production leaves those options unset. Final image hashes differ from
the initial `8267cf0` builds because ESP-IDF embeds the source commit in app
version metadata; all size metrics and BIN lengths are unchanged.

CI for final source `31e4a26` passed host, firmware and Host ASan + UBSan checks
in [Actions run 41448746065](https://github.com/Yuke-hd/mazda-can-accessory-controller/actions/runs/41448746065).
The selector commands from PR #411 remain unchanged in this integration. Their
host configure and ESP-IDF `set-target esp32` checks passed with the two-source
synthetic mapping during that documentation review. No bench, vehicle, flash or
hardware validation was performed.
