# NimBLE resource budget

This records the issue #161 spike that sized NimBLE for the companion BLE
link ([ADR-0001](../decisions/0001-ble-companion-transport.md),
[BLE protocol](../specs/companion/ble-protocol.md)) on the WeAct CAN485 V1.1
firmware. The spike added a connectable advertiser with no companion GATT
service, pairing flow or bench timing. Size figures are build evidence;
heap and task figures come from one idle bench board. Neither is vehicle
validation.

## Method

All images were built with ESP-IDF `v5.5.4` for `esp32` from the same
revision, using `sdkconfig.defaults` plus one overlay:

| Variant | Overlay | Purpose |
| --- | --- | --- |
| Control | none | Spike source with Bluetooth disabled; within 80 bytes of the unmodified firmware image. |
| Tuned | [recommended settings](#recommended-sdkconfig) | Candidate companion configuration. |
| Default | `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y` | ESP-IDF NimBLE defaults, for comparison. |

Static figures come from `idf.py size` and from
`python -m esp_idf_size --archives --diff` on the map files. Heap and task
figures come from separate diagnostic images that also set
`CONFIG_FREERTOS_USE_TRACE_FACILITY=y`. Each logged
`heap_caps_get_free_size()` and `uxTaskGetSystemState()` at 10, 40 and
70 seconds after boot. Readings were stable across the three samples.

The bench board was powered over USB only, with no CAN bus, LED strip or BLE
central attached. It reported an ESP32-D0WD-V3 revision v3.1 with 8 MB of
flash. The images keep the project's 4 MB flash setting, and the hardware
record does not state a flash size, so the budget below assumes 4 MB.

## Static size

| Measure | Control | Tuned | Change |
| --- | ---: | ---: | ---: |
| Application `.bin` | 404,208 B | 678,304 B | +274,096 B |
| Flash `.text` + `.rodata` | 325,438 B | 548,882 B | +223,444 B |
| Static IRAM used | 66,283 B | 107,775 B | +41,492 B |
| Static IRAM remaining | 64,789 B | 23,297 B | −41,492 B |
| Static DRAM used (`.data` + `.bss`) | 54,664 B | 67,980 B | +13,316 B |
| DRAM region size | 180,736 B | 124,580 B | −56,156 B |
| Static DRAM remaining | 126,072 B | 56,600 B | −69,472 B |
| Free space in 1 MiB `factory` | 61% | 35% | −26 points |

The DRAM region shrinks because the ESP32 Bluetooth controller reserves
56,156 B of DRAM at link time when Bluetooth is enabled.

New archives account for most of the increase. Image bytes count flash
code and read-only data plus the IRAM code and initialised `.data` that are
loaded from the image, so they exceed the `.text` + `.rodata` figures above:

| Archive | Image bytes | Static RAM (of which IRAM) |
| --- | ---: | ---: |
| `libbt.a` (NimBLE host and port) | 90,684 B | 7,272 B (1,747 B) |
| `libbtdm_app.a` (controller) | 75,755 B | 25,921 B (22,299 B) |
| `libphy.a` | 43,670 B | 10,347 B (9,122 B) |
| `libcoexist.a` | 9,816 B | 3,966 B (3,333 B) |
| `librtc.a` | 2,016 B | 2,020 B (2,016 B) |

The new archives add 221,941 of the 274,092 image bytes. Existing archives
account for the remaining 52,151 B. The largest increases are in
`libesp_app_format.a` `.rodata`, `libc.a`, `libesp_hw_support.a`,
`libesp_timer.a` and `libesp_phy.a`.

Compared with the tuned build, ESP-IDF defaults add 9,584 B of flash and use
96 B less static DRAM. Tuning matters mainly for core placement and runtime
pools, not for image size.

## Heap at idle

Internal heap is reported after startup, with CAN acquisition running and
BLE advertising when enabled:

| Image | Internal free | Internal minimum | Largest internal block | 8-bit free |
| --- | ---: | ---: | ---: | ---: |
| No BLE | 293,068 B | 293,036 B | 110,592 B | 229,776 B |
| Tuned NimBLE | 155,332 B | 155,232 B | 110,592 B | 134,752 B |
| Default NimBLE | 152,924 B | 152,876 B | 110,592 B | 132,348 B |

Tuned NimBLE costs 137,736 B of internal heap and 95,024 B of byte-addressable
heap. The rest of the internal-heap cost is IRAM, which the BT and PHY
libraries consume (the IRAM-only heap region shrinks from 62 KiB to 20 KiB).
The tuned settings save only 2,408 B of heap compared with the defaults.
Neither build changes the largest free block.

These figures do not cover a connected central, the companion GATT database,
pairing, MTU exchange, notification traffic or configuration transfer. All of
these allocate further heap and must be measured with the GATT
implementation.

## Partition and NVS headroom

The measurements below used the ESP-IDF default single-app table: a 24 KiB
`nvs` partition, a 4 KiB `phy_init` partition and a 1 MiB `factory`
application. The firmware has since adopted the recommended layout below
(64 KiB `nvs`, 1,920 KiB `factory`); see
[firmware builds](firmware-build.md#partition-layout).

**Application.** The NimBLE stub leaves 35% (370,272 B) of the 1 MiB
`factory` partition free. This is enough for the companion GATT work, but the
margin is not large.

**NVS.** NVS is the binding constraint. Once Bluetooth is enabled, three users
share the 24 KiB partition:

- The configuration store's `mazda_config` namespace: two override slots of
  up to 4 KiB each, plus an in-flight replacement of the inactive slot
  ([controller configuration](../specs/configuration/controller-config.md)).
- The PHY calibration record in the `phy` namespace. This is a 1,904-byte
  `cal_data` blob plus version and MAC keys. Default
  `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y` stores it on first boot; the
  bench log shows it being saved.
- NimBLE bonds in the `nimble_bond` namespace. Each of the four bonds stores
  our and peer security records, a peer address record and up to four CCCD
  records (Config Status, legacy Live signals, Service Changed and
  extensible Live signals) as separate blobs.

Estimated against the 630 entries available on five usable 4 KiB pages:

| User | Estimated entries |
| --- | ---: |
| Configuration slots, worst case during replacement | ~390 |
| PHY calibration | ~66 |
| Four NimBLE bonds with up to four CCCDs each | ~112–132 |
| Total (rounded from ~568–588) | ~570–590 |

This leaves little room for page fragmentation, garbage collection, or a
future namespace. When NVS is exhausted, a configuration commit fails with
`ESP_ERR_NVS_NOT_ENOUGH_SPACE`. This estimate is not a measured fill test.

**Recommendation (adopted).** Grow `nvs` in place before companion BLE ships.
The layout below keeps `nvs` at its default `0x9000` offset and only extends it:

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x10000
phy_init, data, phy,     0x19000,  0x1000
factory,  app,  factory, 0x20000,  0x1E0000
```

This gives 64 KiB of NVS (15 usable pages, about 1,890 entries, against the
~570–590 estimate above) and a 1,920 KiB application. It stays inside 4 MB
and leaves 0x200000–0x3FFFFF free for a later OTA layout without moving
`nvs`. Any OTA mechanism needs its own decision; ADR-0001 excludes Wi-Fi.

Existing entries are expected to survive the first serial flash of this
layout without an erase. The ESP-IDF NVS loader (`nvs_page.cpp`,
`nvs_pagemanager.cpp`) treats the added sectors, which hold old `phy_init`
and application bytes, as corrupt free pages and erases each one only when it
first activates it. The original six pages load unchanged. This is from
source inspection and needs bench confirmation. Downgrading is not safe: an
image with the 24 KiB table cannot reach entries that NVS has since written
to the added pages.

## Recommended sdkconfig

Apply after `sdkconfig.defaults`:

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y

# Controller: BLE only, one link, on core 0 (ESP-IDF default; ADR-0004).
CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y
CONFIG_BTDM_CTRL_BLE_MAX_CONN=1
CONFIG_BTDM_CTRL_PINNED_TO_CORE_0=y

# Host: peripheral and broadcaster roles only, one connection, on core 0.
CONFIG_BT_NIMBLE_PINNED_TO_CORE_0=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_GATT_CLIENT=n
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y

# LE Secure Connections, Just Works, bonds persisted in NVS.
CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_SM_LEGACY=n
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_MAX_BONDS=4
# Four bonds × four persisted CCCD settings each (Config Status, legacy
# Live signals, Service Changed and extensible Live signals).
CONFIG_BT_NIMBLE_MAX_CCCDS=16

CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247
# Config transfer commits run in the host task (static estimate, about
# 5.1 KiB worst case; see "Config transfer cost").
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
```

The firmware's `sdkconfig.defaults` applies these settings, including the
security block, which ships with the pairing window (#164). The companion
service keeps pairing closed outside the window at run time: it rejects every
Pairing Request and refuses bond-store writes, so no bond reaches the
`nimble_bond` namespace outside a window (see the
[NimBLE implementation notes](../specs/companion/ble-protocol.md#nimble-implementation-notes)).

The tuned images used ESP-IDF's default of eight CCCD slots. The project set
the capacity to 12 after measurement; under ESP-IDF's
`CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y`, NimBLE allocates its store structure
from internal heap at initialization, so the 8-to-12 increase reserves
64 B there. The extensible Live signals characteristic adds one persisted
CCCD per bonded peer, so four peers need 16 slots. NimBLE's 16-byte CCCD
records make the 12-to-16 increase another 64 B of internal heap allocation.
The firmware build comparison below records linked image and static DRAM
measurements; the heap allocation is derived from the NimBLE source and was
not measured on hardware.

`CONFIG_BT_NIMBLE_SM_SC_ONLY` stays at its default of `0`. The protocol spec
requires Secure Connections without authenticated (MITM) keys, which
SC-only mode would enforce. The ESP32 does not support the BLE 5 feature set,
so no extended-advertising options apply.

## Task and core placement

ADR-0004 (Proposed) records the core layout. The candidate is layout C:

- `app_main` applies the board safe defaults on core 0.
- It then runs the vehicle I/O startup on a short-lived task pinned to
  core 1, so the TWAI, RMT and SPI interrupts are installed there.
- The Bluetooth controller, the NimBLE host and `esp_timer` stay on core 0
  at their ESP-IDF defaults.
- The startup task logs its core and its stack headroom. With
  `CONFIG_WEACT_DUMP_INTERRUPT_ALLOCATION=y` it also prints `esp_intr_dump()`.

The application tasks stay unpinned. They are created inside the startup
task with `xTaskCreate()`, which uses `tskNO_AFFINITY`, so they do not
inherit its pinning.

The table below was measured under layout B (PR #173): BT pinned to core 1,
and the interrupts installed from `app_main` on core 0. Layout C numbers are
pending the bench comparison (#175, alongside #167). Under layout C,
`app_main` no longer runs the vehicle I/O startup, so its `main` row is
expected to have more headroom. The new `vehicle_io_start` task (6,144 B) is
freed once startup completes.

| Task | Priority | Core | Idle stack free (tuned) |
| --- | ---: | --- | ---: |
| `argb_guard` | 24 | any | 1,472 B |
| `can_rx` | 23 | any | 3,328 B |
| `btController` | 23 | 1 | 2,056 B |
| `esp_timer` | 22 | 0 | 3,480 B |
| `mazda_notify` | 21 | any | 1,916 B |
| `nimble_host` | 21 | 1 | 1,972 B of 4,096 B |
| `local_argb` | 2 | any | 2,880 B |
| `vehicle_telemetry` | 1 | any | 1,392 B |
| `main` (`app_main`) | 1 | 0 | 388 B of 3,584 B (392 B without BLE) |
| `ipc0` / `ipc1` | 24 | 0 / 1 | 472 B / 288 B of 1,024 B |

The ESP32 controller's interrupts follow `CONFIG_BTDM_CTRL_PINNED_TO_CORE`.
Its high-level interrupt and ISR queue paths assert that they run on that
core, and it uses inter-processor calls to reach it. Under layout B this cut
`ipc1` headroom from 464 B to 288 B. Under layout C the controller is back on
core 0, as in the ESP-IDF defaults.

Recommendations:

- Keep the controller and host at their ESP-IDF default of core 0, and keep
  the TWAI, RMT and SPI installs on the core-1 startup task. If the bench
  shows layout B is as good or better, ADR-0004 flips to B: the startup
  task is dropped and both BT pinning options return to core 1.
- Keep the BT task priorities at the ESP-IDF defaults. The controller is
  timing-critical. `argb_guard` already outranks every BT task, and `can_rx`
  shares the controller's priority while remaining free to run on core 0.
- Run companion GATT and configuration work in the NimBLE host task or at a
  lower priority, and never block it on lighting or CAN work. BLE failure
  must not delay CAN acquisition or LED fail-off.
- Start BLE after startup black, NVS initialization and CAN start, and log
  and continue on failure. The spike did this, and start-up completed in the
  same order without errors. No CAN bus was attached, so this is not CAN
  receive evidence.
- Do not grow `app_main`'s stack use for BLE. `main` has only 392 B free at
  its high-water mark without BLE. The advertiser stub costs 4 B, but GATT
  registration and store setup will cost more. Start BLE from its own task or
  the host task, or raise `CONFIG_ESP_MAIN_TASK_STACK_SIZE` with a measured
  margin.
- With `CONFIG_BT_NIMBLE_USE_ESP_TIMER=y`, NimBLE callout timers fire in the
  `esp_timer` task at priority 22 on core 0, above `mazda_notify`, and hand
  off to the host task. Measure
  whether `CONFIG_ESP_TIMER_TASK_AFFINITY_CPU1=y` is worth adopting with the
  GATT implementation. It is not part of the candidate settings because it
  was not measured. It also requires `CONFIG_ESP_TIMER_SHOW_EXPERIMENTAL=y`
  (otherwise an overlay setting is silently dropped), and its help text warns
  that it may break other features.
- Re-measure `main`, `nimble_host`, `ipc0` and `ipc1` stack headroom with a
  connected central and the GATT service under layout C. Also record the
  `vehicle_io_start` headroom log. If an IPC task drops below its layout B
  margin, raise `CONFIG_ESP_IPC_TASK_STACK_SIZE`.

## Config transfer cost (#165)

The Config, Config status and Command characteristics run every config commit
inside the Config write handler, on the NimBLE host task. A commit parses the
upload with cJSON, dry-runs `apply_controller_config()` on a scratch engine
and LED sink, re-parses and serializes the upload in `save_override()`, and
writes NVS. This is software evidence only; nothing here was measured on
hardware.

- `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` rises from 4,096 B to 8,192 B.
  The size rests on a static call-graph estimate, not a measurement; see
  "Host stack estimate" below. The handler logs the host stack's remaining
  headroom after each transfer state change, which includes every commit.
- The companion startup task stack rises from 4,096 B to 6,144 B, because it
  now serializes the active configuration once. It logs its own headroom.
- `idf.py size` against the #164 head: static DRAM +6,456 B (`.bss`
  +6,392 B, `.data` +64 B), IRAM unchanged, flash +11,328 B, image
  +11,392 B. The largest additions are all static: the 4.2 KiB transfer
  state with its upload buffer, the 512 B write buffer, and the 1.9 KiB
  scratch engine and LED sink used for the dry run. GCC puts a class with
  default member initializers in `.data` even when every byte is zero, so
  the large buffers are separate statics that land in `.bss`.
- The canonical serializer now formats floats with `snprintf("%.*g")`
  instead of `std::ostringstream`. The output bytes are the same, and the
  firmware no longer links the C++ locale and stream machinery the stream
  pulled in (about 190 KB of flash).

### Host stack estimate

The commit path's translation units (the companion BLE, config, protocol and
controller-config sources, cJSON, `nvs_flash`, `esp_partition`, `spi_flash`,
`heap` and the NimBLE host) were recompiled with the firmware's own compile
commands plus `-fstack-usage -fcallgraph-info=su` (xtensa-esp32-elf GCC from
ESP-IDF 5.5.4). The worst path was found by walking the call graph from the
ATT write handler. Indirect calls GCC cannot resolve were added by hand from
the source: the GATT access callback, the `ConfigCommitter`, `ApplyCheck` and
`ConfigStoreBackend` virtuals, the NVS handle virtuals and every `nvs::Page`
call into the `NVSPartition` read, write and erase virtuals. No frame on the
graph is dynamic.

| Segment | Frames (B) |
| --- | ---: |
| Host receive path down to the ATT handler (`ble_hs_event_rx_data` to `ble_att_rx_extended`) | 240 |
| `ble_att_svr_rx_write` to `access_config` (GATT dispatch) | 256 |
| `access_config` to `ConfigService::commit` | 976 |
| `ConfigStore::save_override` re-parse: `parse_controller_config` and the deepest field parser chain into `std::string` growth | 2,896 |
| Compiled total | 4,368 |
| Not compiled, allowed for: `operator new`, newlib `malloc` and `heap_caps_malloc_default` (272 B measured) | about 340 |
| Not compiled, allowed for: task entry, `nimble_port_run` and the event dispatch | about 100 |
| Not compiled, allowed for: an interrupt or window-spill frame on the task stack | about 400 |
| Estimated worst case | about 5.1 KiB |

The other branches are shallower. From `access_config`, the NVS write
(`write_override` into `nvs_set_blob`, the page compare and erase, and
`esp_partition_write`) peaks at about 3.1 KiB, and the dry run at about
1.9 KiB. cJSON's parser and `cJSON_Delete` recurse, but
`parse_controller_config()` rejects any document nested deeper than 16
levels before it calls cJSON (`kMaxControllerConfigJsonNesting`). At 32 B
per `parse_value` and 32 B per `parse_array` or `parse_object` frame, the
recursion adds at most about 1.1 KiB, so the cJSON path stays below the
field-parser path.

This leaves about 3 KiB of the 8,192 B stack as margin. It is a static
estimate with hand-resolved indirect calls and allowances for code that was
not compiled. It is not a high-water measurement.

**Deferred (bench):** measure the host task's high-water mark on hardware.
Upload a 4 KiB document with maximum nesting and the longest field paths,
commit it to a nearly full NVS partition so that a page erase runs, and
read the headroom the handler logs. Tune the stack from that measurement.

## Extensible Live signals CCCD capacity (#202)

The CCCD store is shared across bonded peers. Each peer can retain four
notification or indication settings: Config Status, legacy Live signals,
Service Changed and extensible Live signals. Four bonds therefore require 16
CCCD records. If all 12 existing slots are occupied, a subscription to the
extensible characteristic can exceed store capacity and fail to persist its
setting. NimBLE reports store-capacity
overflow to the application; this firmware only allows overflow cleanup
while the pairing window is open, preserving stored bonds outside that window.
Keep `CONFIG_BT_NIMBLE_MAX_CCCDS` at 16 together with the four-bond limit.

ESP-IDF 5.5.4 builds for `esp32` on the #201 source at `bb00ce8` measured the
effect of increasing the store from 12 to 16. Both builds used separate fresh
build directories and the same factory profile:

| CCCD slots | Store struct allocation | `.text` | `.bss` | `.data` | Static DRAM | Total image | Application `.bin` |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 12 | 1,104 B | 455,626 B | 49,520 B | 27,564 B | 77,084 B | 701,721 B | 701,840 B |
| 16 | 1,168 B | 455,630 B | 49,520 B | 27,564 B | 77,084 B | 701,725 B | 701,840 B |
| Change | +64 B | +4 B | 0 B | 0 B | 0 B | +4 B | 0 B |

The config-store allocation sizes come from the NimBLE `ble_store_config_vars_t`
definition compiled with each generated configuration. Its four additional
16-byte `ble_store_value_cccd` records add 64 B to the runtime internal-heap
allocation made by `nimble_platform_mem_calloc()` at initialization. They do
not change linked `.bss` or `.data`; the 4-byte `.text` difference is the only
measured image-size change, and the application binary remains the same
padded size. This is software/build evidence only, not a hardware heap
measurement; subscriptions, CCCD persistence and reconnect behavior still
need phone and physical checks.

## Discovery and extensible streaming integration (#205)

The full image was rebuilt from the #204 base (`47241fc`) with ESP-IDF `v5.5.4`,
target `esp32`, the default factory profile, the 16-slot CCCD setting, and a
fresh out-of-tree build directory. The #205 change covers host tests and these
development notes, so it adds no firmware code. `idf.py size` reported:

| Measure | #204 firmware image |
| --- | ---: |
| Flash `.text` | 463,746 B |
| Flash `.rodata` | 93,604 B |
| Static IRAM used / remaining | 124,911 B / 6,161 B |
| Static DRAM `.bss` | 52,024 B |
| Static DRAM `.data` | 27,660 B |
| Static DRAM used / remaining | 79,684 B / 44,896 B |
| Total image size | 710,209 B |
| Application `.bin` | 710,320 B (`0xAD6B0`) |
| Free in 1,920 KiB `factory` partition | 1,255,760 B (`0x132950`, 64%) |

The ESP-IDF Xtensa linker map reports the persistent extensible stream object
as 2,112 B in `.bss` and the optional `SignalCatalogExport` as 268 B in `.bss`.
These are part of the static DRAM total above. The stream retains one accepted
record and timestamp per catalog handle; it does not allocate a task or heap
buffer for discovery or sampling.

The portable limits bound work per operation. Discovery accepts at most 64
Read signals and a 16 KiB document. Each read encodes at most 200 document
bytes plus the 11-byte page header, scans at most 64 descriptors, and the
largest document takes at most 82 pages. Streaming scans at most 64 provider
records per 100 ms sampling slot, attempts at most one notification per slot,
and caps each packet at `min(ATT MTU - 3, 244)` bytes (61 bytes at MTU 64).
Records are appended whole; the next packet resumes from its handle cursor.
Sampling and page encoding run synchronously on the NimBLE host task. The
configured host task stack is 8,192 B. Config commit remains the stack-sensitive
path: its static worst-path estimate is about 5.1 KiB, as detailed in
[Config transfer cost](#config-transfer-cost-165). Page and sample buffers are
fixed-size local objects; this source/compiler review is not a stack
high-water measurement.

**Deferred to #167 (not run):** phone Service Changed/cache refresh, bonded
security and CCCD restoration, notification throughput and packet-loss
behavior, NimBLE host stack high-water, and simultaneous CAN receive plus LED
rendering under BLE traffic. The image, linker map and host tests are software
evidence only; no board was flashed or exercised for these measurements.

## Live signals cost (#166)

The Live signals characteristic samples the read-only provider on the NimBLE
host task through one NimBLE callout and tracks its link through one GAP
event listener. It creates no task and never waits for the stack. This is
software evidence only; nothing here was measured on hardware.

- **GATT.** The legacy Live signals characteristic remains the fifth in the
  existing protocol version. Its CCCD is included in the four-per-bond
  capacity described in
  [Extensible Live signals CCCD capacity](#extensible-live-signals-cccd-capacity-202).
- **Buffers.** Each frame takes one mbuf from the NimBLE pool for as long as
  the stack holds it. When none is free, or the stack refuses the frame, it
  is dropped and the latest state is offered at the next 100 ms slot; frames
  never queue behind each other in the component.
- **Config transfer.** The sampling callout, the GAP listener and the Config
  and Command write handlers all run on the host task, so a sample never runs
  while a commit is in progress and needs no lock. The sampling path stays far
  shallower than the commit path in "Host stack estimate". It neither waits
  for nor delays the write response, the Config status notification or the
  1 s disconnect wait; frames may continue until that disconnect completes,
  and the restart that follows fails the lighting off as before. After Clear
  bonds the next sample sees no accepted bond and sends nothing.
- `idf.py size` against the #175 head (39cfcf9): static DRAM +200 B (`.bss`
  +184 B, `.data` +16 B), IRAM unchanged, flash code +3,056 B, flash data
  +1,008 B, image +4,080 B.

**Deferred (bench):** with a bonded phone subscribed, record the host task
headroom and the frames queued and dropped that the component logs on
disconnect, and the worst-case `LiveSignalSampler::sample()` time.

## Remaining risks

- IRAM is the tightest static resource after NimBLE, at 23,297 B free. If later
  work exhausts it, ESP-IDF options such as
  `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` can move code to flash. They
  trade execution time and have not been evaluated here.
- Radio coexistence, advertising effects on TWAI receive latency, and LED
  timing were not measured. These need bench timing with CAN traffic and an
  LED load before vehicle use.
