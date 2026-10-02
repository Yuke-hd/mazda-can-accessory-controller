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
