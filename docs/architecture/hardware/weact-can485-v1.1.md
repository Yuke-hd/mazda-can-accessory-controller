# WeAct CAN485 DevBoard V1.1 record

## Approved product identity

The sole vehicle/product target is the WeAct Studio CAN485 DevBoard V1.1,
represented in code by `board::kWeActCan485V11` and built from
`firmware/weact-can485-v1.1`. The V1.1 revision is mandatory: do not substitute
another revision or board without reviewing the complete capability record.

The V1.1 assembly uses a CA-IS2062A CAN transceiver, a CA-IS2092A RS485
transceiver, and a CH343P native USB serial bridge. The FPC-18 connector carries
the board I/O described below. These identities are release inputs and must be
checked against the physical board before vehicle use.

This record was reviewed on 2026-09-04 against official WeAct Studio material
at commit `0865d397b931602d10bc740aa135b3a2c782340a` (upstream commit date
2026-05-25):

- [Official WeActStudio.CAN485DevBoardV1_ESP32 repository](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/tree/0865d397b931602d10bc740aa135b3a2c782340a)
- [Pinout and V1.1 revision record](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/blob/0865d397b931602d10bc740aa135b3a2c782340a/README.md)
- [V1.1 SchDoc.pdf](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/blob/0865d397b931602d10bc740aa135b3a2c782340a/Hardware/WeAct-CAN485DevBoardV1_ESP32_V1.1%20SchDoc.pdf)

## Central capability record

| Capability | GPIO | Startup handling |
| --- | ---: | --- |
| Classic CAN RX | 26 | Input |
| Classic CAN TX | 27 | Recessive latch before TWAI startup |
| RS485 DE | 17 | Output low; RS485 driver disabled |
| RS485 RO | 21 | Input |
| RS485 DI | 22 | Output high while DE remains inactive |
| microSD MISO | 2 | Input until a storage driver owns it |
| microSD MOSI | 15 | Input until a storage driver owns it |
| microSD SCLK | 14 | Input until a storage driver owns it |
| microSD CS | 13 | Input until a storage driver owns it |
| Onboard WS2812 status pixel | 4 | Output low; one pixel |
| Vehicle WS2812B strip data | 16 | Output low; up to 300 pixels, 100 by default |
| VIN sense | 36 | Input-only |
| User key | 0 | Input |

The CH343P connection is modelled as the board's native USB serial interface;
it has no application GPIO assignment. The `board` component is the sole
source of the vehicle pin map. Decoder and domain-library code must remain
hardware independent.

The CA-IS2062A is always powered. This board has no software-controllable CAN
power, standby, speed-select, or boost pin, and the shared vehicle components
must not expose an API for one. `initialize_safe_defaults()` only establishes
safe GPIO direction and levels; it does not switch transceiver power.

### Vehicle LED strip length

The board maximum for the GPIO16 strip is 300 pixels
(`kVehicleLightStripPixelCount`). The user sets the attached length, from 100
to 300, with `sinks.local_argb.pixel_count`; the default is 100. See
[ADR-0007](../../decisions/0007-configurable-strip-length.md) and the
[controller configuration](../../specs/configuration/controller-config.md#sink-settings).
Startup black, fail-off black and the black written before a restart must
cover all 300 pixels at any configured length.
[#289](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/289)
and [#291](https://github.com/Yuke-hd/mazda-can-accessory-controller/issues/291)
implement this; until they merge, the firmware drives a fixed 100-pixel
strip.

Only the 100-pixel length is the reviewed configuration. Lengths above 100 are
**unverified** and not supported for vehicle use until a reviewed power
measurement records, at the configured length and the renderer's brightness
ceiling, the strip supply current, the voltage at the far end of the strip
and the supply wiring and fuse used. The same review must record the
full-length refresh time against the driver hang watchdog
(`kDriverHangRestartUs`). Host tests and a firmware build establish none of
these.

### SPI flash

The firmware configures 4 MB of SPI flash in DIO mode at 80 MHz
(`sdkconfig.defaults`); see [flash-cache layout](../../development/flash-cache-layout.md).
The bench board's flash reports JEDEC manufacturer `0xC8` (GigaDevice) and
device `0x4017`. The project owner approved 80 MHz DIO without a cited
flash-part datasheet. The ESP32 supports 80 MHz flash, and the bench board's
second-stage bootloader reported `SPI Speed: 80MHz` and `SPI Mode: DIO` and ran
under CAN load. That is bench evidence for one board, not a part-number
datasheet rating. Confirm the flash part on any other V1.1 board before vehicle
use.

## Vehicle safety boundary

Before connecting to a vehicle:

1. Confirm the board marking is WeAct CAN485 DevBoard V1.1 and verify the
   CA-IS2062A, CA-IS2092A, CH343P, and FPC-18 assembly identities.
2. Put termination switch K3 in the **OFF** position. Do not add another
   termination resistor to an already terminated vehicle bus.
3. Verify CAN H, CAN L, ground, and protected power wiring against the physical
   V1.1 board and vehicle integration plan.
4. Confirm the GPIO4 and GPIO16 data lines remain low at reset and startup. This prevents new
   command pulses but does not clear a color latched across a warm reset or
   failure. Sending an RMT-encoded black frame is required to clear the physical
   pixel; the #16 `local_argb` owner sends it before CAN startup. Under
   ADR-0007 that frame must cover the full 300-pixel maximum.
5. Build only `weact_can485_v11_vehicle_listen_only` for vehicle use and confirm
   `TWAI_MODE_LISTEN_ONLY` with a zero-length TX queue.
6. Use a strip length above 100 pixels only after the power measurement in
   [Vehicle LED strip length](#vehicle-led-strip-length) is reviewed.

Native USB serial logs through the CH343P may be retained as auxiliary startup
and counter evidence. Logs are not proof of electrical no-ACK behavior and
must contain no raw vehicle payload, VIN, credentials, location, absolute
timestamp, or reconstructable trip information.

## Evidence status and limitations

The host tests and structural validators prove the checked pin record, product
identity, build separation, listen-only configuration, zero TX queue, lack of a
public or driver data-frame transmit call, and the GPIO4/GPIO16 output latch requests.
MCAN-16 additionally sends a checked RMT black frame before CAN startup. These
checks do not prove physical LED behavior. CI compiles the WeAct vehicle
firmware with ESP-IDF 5.5.4.

This change does **not** claim a physical V1.1 continuity inspection, protected
power test, K3 measurement, termination measurement, CAN analyzer no-ACK trace,
ignition-cycle test, or vehicle test. Record those results separately before a
vehicle release. Any active-CAN or acknowledgement experiment belongs in a
physically isolated external bench setup and must never be added to this
vehicle target.
