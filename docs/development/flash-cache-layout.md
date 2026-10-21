# Flash-cache layout

The firmware pins the hot telemetry and CAN receive paths' code to IRAM and
the project components' read-only data to DRAM. This page explains why, what
the build does, and how to keep it working when the hot functions change.

## Problem

The original ESP32 executes code in place from flash through a 32 KiB per-core
cache that instructions and data share. Before this change about 89% of
telemetry-path cycles were cache stalls. Changing an unrelated Kconfig option
moved hot `.flash.text` relative to `.flash.rodata` and caused conflict misses,
so whichever build received the bad layout was 3-4x slower. Compiler options
alone (for example `-O2`) flipped which build was slow.

Before the change, a bench A/B measured about 2,450 fps at 257 µs/frame with
`CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y` and 560-775 fps at 783-1,082 µs/frame
with it disabled (bench evidence, not vehicle validation).

The receive path had the same problem after the telemetry fix. A bench
CAN-load comparison measured `can_rx` at about 49 µs/frame with freshness
debug disabled and about 60 µs/frame with it enabled, while telemetry stayed
at about 96 and 124 µs/frame respectively (bench evidence from the experiment
build, not vehicle validation). Pinning the receive path removes that
layout-dependent flash execution.

## What the build does

All of this is unconditional for `firmware/weact-can485-v1.1`. Host builds of
`lib/` and the tests are unaffected.

| Mechanism | Location | Effect |
| --- | --- | --- |
| `-O2` | `firmware/weact-can485-v1.1/CMakeLists.txt` | Compiles `mazda_telemetry`, `mazda`, `vehicle_telemetry`, `vehicle_core`, `vehicle_signals`, `action_engine`, `vehicle_lighting_policy`, `vehicle_can_rx`, `can_bus`, `local_argb`, and `local_argb_actions` with `-O2` (interface libraries are skipped). The pinned vehicle-core components are compiled with these options from this project; their sources are not modified. |
| `hot_rodata.lf` | `firmware/weact-can485-v1.1/main/` | `noflash_data` moves those archives' and `libmain.a`'s `.rodata` to DRAM, removing the data side of the cache conflict. |
| `hot_code.lf` | `firmware/weact-can485-v1.1/main/` | `noflash` places the per-frame `VehicleTelemetryService`, `can_bus`, `CanBusSource`, TWAI task API, and TWAI HAL functions in IRAM by exact symbol. |
| `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` | `firmware/weact-can485-v1.1/sdkconfig.defaults` | Moves non-ISR IDF ringbuf functions to flash, freeing IRAM for the receive path. The per-frame receive path uses `can_bus`'s fixed SPSC ring, not the IDF ringbuf; ISR ringbuf entry points stay in IRAM. |
| Clone-free flags | `firmware/weact-can485-v1.1/CMakeLists.txt` | `mazda_telemetry` uses `-fno-ipa-sra -fno-ipa-cp-clone -fno-partial-inlining -fno-ipa-cp`. ldgen symbol entries cannot contain the `$` of GCC clone names (`.isra`, `.constprop`, `.part`), so the hot functions must keep their plain symbols. |
| 80 MHz DIO flash | `firmware/weact-can485-v1.1/sdkconfig.defaults` | `CONFIG_ESPTOOLPY_FLASHFREQ_80M` doubles the flash clock from the 40 MHz default, roughly halving the refill time of the cache misses that remain for code and data still in flash. The mode stays DIO. The firmware configure step fails if a stale `sdkconfig` does not select 80 MHz DIO. |
| `tools/check_iram_placement.py` | POST_BUILD step of the application ELF | Fails the build if any `hot_code.lf` symbol is missing from the ELF or lies outside `.iram0.text`. ldgen silently ignores a symbol entry that matches nothing. |

### CAN receive hot path

`hot_code.lf` pins both halves of the acquisition boundary and the driver
functions they call for every frame:

| Stage | Pinned functions | Archive |
| --- | --- | --- |
| Acquisition task | `can_bus::(anonymous namespace)::receive_task` | `libcan_bus.a` |
| Consumer pop | `can_bus::receive` | `libcan_bus.a` |
| Telemetry adapter | `vehicle_telemetry::CanBusSource::receive` | `libvehicle_telemetry.a` |
| TWAI task API | `twai_receive`, `twai_read_alerts`, `twai_get_status_info`, and their `_v2` implementations | `libdriver.a` |
| TWAI frame HAL | `twai_hal_parse_frame` | `libhal.a` |
| Frame validation | `vehicle_core::RawCanFrame::is_valid` | `libvehicle_core.a` |

The task entry contains the frame copy, ring push, semaphore give, status
poll, and alert poll. It calls `RawCanFrame::is_valid` across a translation
unit boundary, so that validation function is pinned separately rather than
inlined. `can_bus::receive` contains the ring pop. The IDF wrappers are pinned
together with their `_v2` targets because each wrapper forwards to the `_v2`
implementation. `twai_hal_get_tec` and `twai_hal_get_rec` are not pinned:
`twai_get_status_info_v2` only calls them outside listen-only mode, which this
firmware never enters. `memcpy`/`memset` resolve to ROM and the FreeRTOS/ESP
timer functions used by this path are already IRAM-resident through existing
configuration.

Because the checker is part of the ELF build, `idf.py build` (including the CI
firmware job) enforces it. To run it by hand:

```sh
python3 tools/check_iram_placement.py \
  --fragment firmware/weact-can485-v1.1/main/hot_code.lf \
  --elf <build-dir>/weact_can485_v11_vehicle_listen_only.elf \
  --nm xtensa-esp32-elf-nm --objdump xtensa-esp32-elf-objdump
```

### Debugging at a lower optimisation level

The `-O2` option is added after the global
`CONFIG_COMPILER_OPTIMIZATION_*` level, so it overrides menuconfig for the
listed components. Choosing Debug (`-Og`) or None (`-O0`) in menuconfig does
not change them, and GDB shows optimised-out variables and inlined frames in
that code. To debug these components at a lower level, temporarily remove
`-O2` from the loop in `firmware/weact-can485-v1.1/CMakeLists.txt`. Such a
build has different IRAM usage and telemetry performance; the worst-case
profiling configuration may no longer link. Do not ship or vehicle-test it,
and restore `-O2` before committing.

## Updating `hot_code.lf`

When a hot telemetry, `can_bus`, TWAI, or adapter function is added, renamed,
or changes signature, the checker fails and lists the missing symbol. To
regenerate the entries:

1. Keep the clone-free flags on `mazda_telemetry`. Without them GCC may emit
   only a `.isra`/`.constprop`/`.part` clone, which ldgen cannot name.
2. Build the firmware, then list the candidate symbols from the component
   archive:

   ```sh
   xtensa-esp32-elf-nm --defined-only \
     <build-dir>/esp-idf/mazda_telemetry/libmazda_telemetry.a \
     | grep VehicleTelemetryService | grep ' [Tt] '
   ```

   Pipe the names through `xtensa-esp32-elf-c++filt` to identify functions.
   Names containing `.` or `$` are clones; remove the cause rather than
   listing them.
3. Write each entry as `vehicle_telemetry:<mangled-name> (noflash)`, where
   `vehicle_telemetry` is the object file (`vehicle_telemetry.cpp.obj`).
4. List the CAN receive symbols one archive at a time, so the archive name
   identifies the group:

   ```sh
   for archive in \
     <build-dir>/esp-idf/can_bus/libcan_bus.a \
     <build-dir>/esp-idf/vehicle_telemetry/libvehicle_telemetry.a \
     <build-dir>/esp-idf/driver/libdriver.a \
     <build-dir>/esp-idf/hal/libhal.a \
     <build-dir>/esp-idf/vehicle_core/libvehicle_core.a; do
     echo "== $archive"
     xtensa-esp32-elf-nm --defined-only "$archive" \
       | grep -E 'receive_task|CanBusSource7receive| twai_(receive|read_alerts|get_status_info|hal_)|RawCanFrame8is_valid'
   done
   ```

   Find the object name with `xtensa-esp32-elf-ar t <archive>` (for example
   `can_bus.cpp.obj`). Drop the extension and use that object label before the
   symbol (`can_bus`, `can_bus_source`, `twai`, `twai_hal_v1`, or
   `vehicle_core`). If the compiler emits a clone instead, apply the
   clone-free flags from this project to that component rather than listing
   the clone.
5. Rebuild and confirm `IRAM placement check passed` in the build output, then
   re-check the IRAM headroom below.

Keep the list to functions on the per-frame path. Every entry consumes IRAM.

## IRAM and DRAM headroom

ESP32 static IRAM is the binding constraint. Measured in bytes with
`python -m esp_idf_size <build-dir>/weact_can485_v11_vehicle_listen_only.map`
for fresh builds of this layout with 80 MHz DIO flash
(ESP-IDF 5.5.4):

| Build | `SDKCONFIG_DEFAULTS` | IRAM used / free | DRAM used / free |
| --- | --- | --- | --- |
| Production | `sdkconfig.defaults` | 125,703 / 5,369 | 81,948 / 42,632 |
| Worst-case profiling | `sdkconfig.defaults;sdkconfig.runtime-stats.defaults` plus `CONFIG_WEACT_CAN_TELEMETRY_PROFILING=y` and `CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y` | 130,411 / 661 | 90,188 / 34,392 |
| Synthetic benchmark | `sdkconfig.defaults;sdkconfig.runtime-stats-synthetic.defaults` | 74,363 / 56,709 | 56,684 / 67,896 |

The worst-case profiling build leaves 661 B of IRAM. Moving the non-ISR IDF
ringbuf helpers to flash paid for most of the receive-path placement; without
that option the receive-path entries did not fit this image. Before adding
symbols to `hot_code.lf` or enabling Kconfig options that place code in IRAM,
rebuild that configuration and confirm it still links. If it does not, move
other non-ISR code to flash or trim the hot list; do not remove a per-frame
symbol without measuring the result.
