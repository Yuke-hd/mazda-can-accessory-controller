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

On macOS and Linux, source the SDK's `export.sh`; on other supported hosts,
activate ESP-IDF `v5.5.4` using Espressif's setup procedure before running the
same toolchain check and build commands. The checker reports missing tools
but does not install them or change system packages.

These commands establish compilation only. They do not establish CAN
listen-only behavior under vehicle conditions, LED timing, wiring, termination,
or physical vehicle acceptance. Report firmware and hardware verification
separately from host tests.
