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
| Vehicle WS2812B strip data | 16 | Output low; 100-pixel strip |
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
   pixel; the #16 `local_argb` owner sends it before CAN startup.
5. Build only `weact_can485_v11_vehicle_listen_only` for vehicle use and confirm
   `TWAI_MODE_LISTEN_ONLY` with a zero-length TX queue.

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
