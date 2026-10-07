# Firmware builds

The WeAct CAN485 DevBoard V1.1 application targets ESP32 with ESP-IDF
`v5.5.4`. The firmware manifest requires that exact SDK version; it does not
fetch an unrelated SDK during configuration.

Activate the installed SDK and check the firmware toolchain from the
repository root. Replace the placeholder with the ESP-IDF installation path on
the current host:

```sh
source /path/to/esp-idf/v5.5.4/export.sh
python3 tools/check_toolchain.py --scope firmware
```

Then build the application:

```sh
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
```

The factory profile defaults to `config/default.yaml`. To build with a local
profile, place it in `config/` (git-ignored apart from `default.yaml`) and pass
`-DCONTROLLER_CONFIG_DEFAULT_YAML=<path>` to `idf.py`.

Configuration prints the selected `CONTROLLER_CONFIG_DEFAULT_YAML` path and
the SHA256 of its generated factory JSON. The YAML selection is a CMake cache
entry: an explicit `-D` override persists in that build directory until changed
or cleared. Check both provenance lines before using an image. Changes to the
YAML, compiler, or signal catalog trigger reconfiguration and a new hash.
The controller-config component rejects an unset or missing generated JSON;
ESP-IDF compilation also requires the embedding definition.

On macOS and Linux, source the SDK's `export.sh`; on other supported hosts,
activate ESP-IDF `v5.5.4` using Espressif's setup procedure before running the
same toolchain check and build commands. The checker reports missing tools
but does not install them or change system packages.

These commands establish compilation only. They do not establish CAN
listen-only behavior under vehicle conditions, LED timing, wiring, termination,
or physical vehicle acceptance. Report firmware and hardware verification
separately from host tests.

For the bounded, opt-in investigation of `0x091` freshness incidents, use the
[CAN freshness debug runbook](can-freshness-debug.md). It keeps the production
freshness and receive-only policies unchanged.

## Partition layout

The firmware uses a custom single-app partition table,
`firmware/weact-can485-v1.1/partitions.csv`, for 4 MB flash:

| Name | Type | Offset | Size |
| --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | 64 KiB (`0x10000`) |
| `phy_init` | data/phy | `0x19000` | 4 KiB (`0x1000`) |
| `factory` | app/factory | `0x20000` | 1,920 KiB (`0x1E0000`) |

`0x200000`–`0x3FFFFF` is intentionally unallocated. Print the built table with
`idf.py partition-table`. The sizing rationale is in the
[BLE resource budget](ble-resource-budget.md#partition-and-nvs-headroom).

**Existing build directories.** `sdkconfig.defaults` only fills options that
the git-ignored `sdkconfig` does not already set, so a checkout configured
before this layout keeps the old default table. Run `idf.py set-target esp32`
(or delete `firmware/weact-can485-v1.1/sdkconfig`) once after pulling, then
confirm that `idf.py partition-table` shows the 64 KiB `nvs` before flashing.
The firmware configure step fails unless `CONFIG_PARTITION_TABLE_CUSTOM` is
set and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` is `partitions.csv`, so a stale
configuration cannot build silently.

`nvs` keeps the ESP-IDF default offset and only grows from the default
24 KiB, so a board flashed with the default table upgrades in place:

- Use a plain `idf.py flash`. Do **not** run `idf.py erase-flash`; it would
  needlessly wipe stored configuration overrides, PHY calibration and BLE
  bonds.
- The NVS loader treats the added sectors, which still hold old `phy_init` and
  application bytes, as corrupt free pages and erases each one only when it
  first uses it. The original pages load unchanged. One WeAct CAN485 V1.1
  bench board confirmed this: an override and the PHY calibration record
  stored under the old table survived the upgrade, and later commits used and
  recycled all 16 pages (PR #176).

**Downgrade hazard.** Do not flash an image that uses the old 24 KiB table
onto a board that has run this layout. Entries that NVS has written to the
added pages become unreachable, so overrides or bonds can disappear. If all
six original pages are in use, `nvs_flash_init` instead fails with
`ESP_ERR_NVS_NO_FREE_PAGES`; the firmware then uses its factory configuration
on every boot because it never erases NVS automatically. A downgrade
therefore needs an explicit NVS erase (for example `idf.py erase-flash` before
flashing), which loses stored overrides and bonds.
