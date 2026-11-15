# Round display ESP-NOW link

Status: implemented behind `CONFIG_DISPLAY_ESPNOW` (default off). Software
builds and host tests only; pairing and streaming with a real display are
**not yet bench validated**.

The controller streams vehicle readings to the round display (the
`esp32-1-75` project) over encrypted ESP-NOW. The display owns the wire
contracts; this controller implements them as pinned there:

- `docs/protocol/esp-now-v1.md` in `esp32-1-75`: ECAN telemetry v1.
- `docs/protocol/pairing-v1.md` in `esp32-1-75`: pairing v1 (display =
  Security1 initiator, controller = responder).

A contract change starts in that project; update the encoder, its test
vectors and this page together.

## Components

| Piece | Location | Role |
|---|---|---|
| ECAN v1 encoder | `lib/companion_protocol/.../display_snapshot.hpp` | 23 records from the read-only `SignalProvider`; brake IDs 19 and 23 never Fresh; stopped packet before telemetry starts |
| Link logic | `lib/companion_protocol/.../display_link.hpp` | Pairing record codec, setup code normalization, 5 s long press, session/sequence publisher |
| ESP-IDF binding | `components/display_espnow/` | Wi-Fi/ESP-NOW streaming, setup boot, NVS |
| SDK adaptation | `components/display_espnow/sdk/` | The display's reviewed overlay on `espressif/esp-now` 2.5.3, applied and SHA-checked at configure time |
| Provisioning | `tools/provision_display_code.py` | Per-controller setup code as NVS CSV |

The binding only reads the signal provider. It never touches CAN, the
lighting engine or the LED renderer, and any failure only loses the display
link. CAN stays listen-only.

## NVS (`display_link` namespace)

| Key | Type | Written by | Content |
|---|---|---|---|
| `setup_code` | string | flash-time provisioning | 16 hex digits; Security1 PoP |
| `record` | blob, 26 B | setup boot | version 1, display MAC[6], channel, profile 1, ECAN 1, LMK[16] |
| `setup` | u8 | long press | 1 = boot into setup mode once |

Keys are never logged. The PMK is random per boot on each device and never
stored; only the LMK (first 16 bytes of the exchanged 32-byte key) is shared.

## Streaming boot

With a valid `record`: Wi-Fi STA on the saved channel (RAM storage, default
power save because BLE coexistence needs modem sleep), the display as the one
encrypted peer, and a 200-byte snapshot every 50 ms. No record, no NVS or any
start failure leaves Wi-Fi off.

## Setup (pairing) boot

1. Hold the user key for 5 s. The controller saves `setup = 1` and takes the
   controlled (fail-off) restart. A short press still only opens the BLE
   pairing window.
2. The setup boot clears the flag first, so any crash returns to streaming.
   Without a valid `setup_code` it skips setup.
3. The SDK responder runs on the saved channel (channel 1 when unpaired) for
   one 120 s window. Three failed attempts or expiry restart normally.
4. On the display, Scan, select the controller and enter the label's code.
5. After the authenticated key exchange and the key-response send attempt,
   the controller saves `record`, erases the SDK's own key copy and restarts
   into streaming. The display verifies the link after its own restart.

## Build and provision

```sh
idf.py -C firmware/weact-can485-v1.1 -B <build> -D SDKCONFIG=<build>/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.espnow.defaults" build
python3 tools/provision_display_code.py   # prints the code and flash commands
```

`sdkconfig.espnow.defaults` also moves Wi-Fi's IRAM speed paths to flash;
BLE plus Wi-Fi otherwise overflow IRAM. Flashing the generated NVS image
replaces the whole NVS partition (saved config, BLE bonds, display pairing).

Measured on 2026-11-15 (estimates; runtime heap with BLE plus Wi-Fi not
measured): on build 1,250,624 B of the 0x1E0000 app partition, IRAM 97.6 %
(3.1 KB free), static DRAM 82.8 %; off build unchanged with no ESP-NOW or
Wi-Fi symbols.

## Bench validation still required

Pairing with a real display, wrong-code and three-failure behaviour, window
expiry, encrypted streaming with independent PMKs, plaintext rejection, BLE
coexistence, runtime heap and stack margins.
