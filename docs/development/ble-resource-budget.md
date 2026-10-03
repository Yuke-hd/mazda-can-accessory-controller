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
  our and peer security records, a peer address record and up to three CCCD
  records (config status, live signals and Service Changed) as separate
  blobs.

Estimated against the 630 entries available on five usable 4 KiB pages:

| User | Estimated entries |
| --- | ---: |
| Configuration slots, worst case during replacement | ~390 |
| PHY calibration | ~66 |
| Four NimBLE bonds | ~100–120 |
| Total | ~560–580 |

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
~560–580 estimate above) and a 1,920 KiB application. It stays inside 4 MB
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

# Controller: BLE only, one link, on core 1.
CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y
CONFIG_BTDM_CTRL_BLE_MAX_CONN=1
CONFIG_BTDM_CTRL_PINNED_TO_CORE_1=y

# Host: peripheral and broadcaster roles only, one connection, on core 1.
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
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
# Four bonds × three CCCDs each. Below this, a full CCCD store makes the
# stock overflow handler unpair the oldest peer outside the pairing window.
CONFIG_BT_NIMBLE_MAX_CCCDS=12

CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
```

`CONFIG_BT_NIMBLE_MAX_CCCDS=12` was added after measurement; the tuned images
used the default of 8. The four extra 16-byte store slots add 64 B of static
RAM.

`CONFIG_BT_NIMBLE_SM_SC_ONLY` stays at its default of `0`. The protocol spec
requires Secure Connections without authenticated (MITM) keys, which
SC-only mode would enforce. The ESP32 does not support the BLE 5 feature set,
so no extended-advertising options apply.

## Task and core placement

The firmware installs the TWAI, RMT and SPI interrupts from `app_main` on
core 0. Its application tasks are unpinned:

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

With ESP-IDF defaults, both `btController` and `nimble_host` run on core 0, the
same core as the TWAI, RMT and SPI interrupts. The recommended settings move
both tasks to core 1, and the bench task list confirms the placement. The
ESP32 controller's interrupts follow `CONFIG_BTDM_CTRL_PINNED_TO_CORE`, so they
also leave core 0: its high-level interrupt and ISR queue paths assert that
they run on that core, and it uses inter-processor calls to reach it. This
explains the lower `ipc1` headroom.

Recommendations:

- Keep the controller and host pinned to core 1.
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
- Re-measure `main`, `nimble_host` and `ipc1` stack headroom with a
  connected central and the GATT service. `ipc1` drops from 464 B to 288 B
  free once BT runs on core 1. If the GATT build reduces it further, raise
  `CONFIG_ESP_IPC_TASK_STACK_SIZE`.

## Remaining risks

- IRAM is the tightest static resource after NimBLE, at 23,297 B free. If later
  work exhausts it, ESP-IDF options such as
  `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` can move code to flash. They
  trade execution time and have not been evaluated here.
- Radio coexistence, advertising effects on TWAI receive latency, and LED
  timing were not measured. These need bench timing with CAN traffic and an
  LED load before vehicle use.
