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

`nvs` keeps the ESP-IDF default offset and only grows from the default
24 KiB, so a board flashed with the default table upgrades in place:

- Use a plain `idf.py flash`. Do **not** run `idf.py erase-flash`; it would
  needlessly wipe stored configuration overrides, PHY calibration and BLE
  bonds.
- The NVS loader treats the added sectors, which still hold old `phy_init` and
  application bytes, as corrupt free pages and erases each one only when it
  first uses it. The original pages load unchanged. This follows from ESP-IDF
  source inspection and still needs bench confirmation.

**Downgrade hazard.** Do not flash an image that uses the old 24 KiB table
onto a board that has run this layout. Entries that NVS has written to the
added pages become unreachable, so overrides or bonds may silently disappear. A downgrade needs an explicit NVS erase (for example
`idf.py erase-flash` before flashing), which loses stored overrides and bonds.
The firmware itself never erases NVS automatically.

These commands establish compilation only. They do not establish CAN
listen-only behavior under vehicle conditions, LED timing, wiring, termination,
or physical vehicle acceptance. Report firmware and hardware verification
separately from host tests.
