# Flash-cache layout

The firmware pins the hot telemetry path's code to IRAM and the project
components' read-only data to DRAM. This page explains why, what the build
does, and how to keep it working when the hot functions change.

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

## What the build does

All of this is unconditional for `firmware/weact-can485-v1.1`. Host builds of
`lib/` and the tests are unaffected.

| Mechanism | Location | Effect |
| --- | --- | --- |
| `-O2` | `firmware/weact-can485-v1.1/CMakeLists.txt` | Compiles `mazda_telemetry`, `mazda`, `vehicle_telemetry`, `vehicle_core`, `vehicle_signals`, `action_engine`, `vehicle_lighting_policy`, `vehicle_can_rx`, `can_bus`, `local_argb`, and `local_argb_actions` with `-O2` (interface libraries are skipped). The pinned vehicle-core components are compiled with these options from this project; their sources are not modified. |
| `hot_rodata.lf` | `firmware/weact-can485-v1.1/main/` | `noflash_data` moves those archives' and `libmain.a`'s `.rodata` to DRAM, removing the data side of the cache conflict. |
| `hot_code.lf` | `firmware/weact-can485-v1.1/main/` | `noflash` places the per-frame `VehicleTelemetryService` functions in IRAM by exact mangled symbol. |
| Clone-free flags | `firmware/weact-can485-v1.1/CMakeLists.txt` | `mazda_telemetry` uses `-fno-ipa-sra -fno-ipa-cp-clone -fno-partial-inlining -fno-ipa-cp`. ldgen symbol entries cannot contain the `$` of GCC clone names (`.isra`, `.constprop`, `.part`), so the hot functions must keep their plain symbols. |
| 80 MHz DIO flash | `firmware/weact-can485-v1.1/sdkconfig.defaults` | `CONFIG_ESPTOOLPY_FLASHFREQ_80M` doubles the flash clock from the 40 MHz default, roughly halving the refill time of the cache misses that remain for code and data still in flash. The mode stays DIO. The firmware configure step fails if a stale `sdkconfig` does not select 80 MHz DIO. |
| `tools/check_iram_placement.py` | POST_BUILD step of the application ELF | Fails the build if any `hot_code.lf` symbol is missing from the ELF or lies outside `.iram0.text`. ldgen silently ignores a symbol entry that matches nothing. |

Because the checker is part of the ELF build, `idf.py build` (including the CI
firmware job) enforces it. To run it by hand:

```sh
python3 tools/check_iram_placement.py \
  --fragment firmware/weact-can485-v1.1/main/hot_code.lf \
  --elf <build-dir>/weact_can485_v11_vehicle_listen_only.elf \
  --nm xtensa-esp32-elf-nm --objdump xtensa-esp32-elf-objdump
```

## Updating `hot_code.lf`

When a hot `VehicleTelemetryService` function is added, renamed, or changes
signature, the checker fails and lists the missing symbol. To regenerate the
entries:

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
4. Rebuild and confirm `IRAM placement check passed` in the build output, then
   re-check the IRAM headroom below.

Keep the list to functions on the per-frame path. Every entry consumes IRAM.

## IRAM and DRAM headroom

ESP32 static IRAM is the binding constraint. Measured in bytes with
`python -m esp_idf_size <build-dir>/weact_can485_v11_vehicle_listen_only.map`
for fresh builds of this layout with 80 MHz DIO flash
(ESP-IDF 5.5.4):

| Build | `SDKCONFIG_DEFAULTS` | IRAM used / free | DRAM used / free |
| --- | --- | --- | --- |
| Production | `sdkconfig.defaults` | 124,911 / 6,161 | 81,204 / 43,376 |
| Worst-case profiling | `sdkconfig.defaults;sdkconfig.runtime-stats.defaults` plus `CONFIG_WEACT_CAN_TELEMETRY_PROFILING=y` and `CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y` | 129,695 / 1,377 | 89,444 / 35,136 |
| Synthetic benchmark | `sdkconfig.defaults;sdkconfig.runtime-stats-synthetic.defaults` | 73,587 / 57,485 | 60,732 / 63,848 |

The worst-case profiling build leaves about 1.3 KiB of IRAM. Before adding
symbols to `hot_code.lf` or enabling Kconfig options that place code in IRAM,
rebuild that configuration and confirm it still links.
