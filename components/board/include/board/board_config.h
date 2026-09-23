#pragma once

#include <cstddef>
#include <cstdint>

namespace board {

// This record is the only board-specific input consumed by firmware assembly.
// vehicle_core and Mazda decoders must not include this header.
enum class HardwareRevision : std::uint8_t {
  kWeActCan485DevBoardV11 = 0,
};

struct CanPins {
  std::int8_t tx;
  std::int8_t rx;
};

struct Rs485Pins {
  std::int8_t driver_enable;
  std::int8_t receiver_output;
  std::int8_t driver_input;
};

struct MicroSdPins {
  std::int8_t miso;
  std::int8_t mosi;
  std::int8_t sclk;
  std::int8_t cs;
};

struct OnboardRgb {
  std::int8_t data;
  std::uint8_t pixel_count;
};

struct VehicleLightStrip {
  std::int8_t data;
  std::uint8_t pixel_count;
};

// The onboard WS2812 is a single physical status pixel. The vehicle light
// strip is a separate logical renderer surface and is intentionally longer.
inline constexpr std::uint8_t kOnboardRgbPixelCount = 1;
inline constexpr std::uint8_t kVehicleLightStripPixelCount = 100;

struct AuxiliaryPins {
  std::int8_t vin_sense;
  std::int8_t user_key;
};

struct UsbSerialInterface {
  const char *transport;
  const char *bridge;
};

struct Capabilities {
  HardwareRevision revision;
  CanPins can;
  Rs485Pins rs485;
  MicroSdPins micro_sd;
  OnboardRgb onboard_rgb;
  VehicleLightStrip vehicle_light_strip;
  AuxiliaryPins auxiliary;
  UsbSerialInterface usb_serial;
  // The CA-IS2062A CAN transceiver is always powered on this board. There is
  // deliberately no controllable transceiver-control pin or board API.
  bool can_transceiver_always_powered;
  // Vehicle firmware is permanently receive-only. There is intentionally no
  // board or CAN API for sending a CAN frame.
  bool vehicle_listen_only;
  bool can_transmit_api_exposed;
};

inline constexpr Capabilities kWeActCan485V11{
    HardwareRevision::kWeActCan485DevBoardV11,
    CanPins{27, 26},
    Rs485Pins{17, 21, 22},
    MicroSdPins{2, 15, 14, 13},
    OnboardRgb{4, kOnboardRgbPixelCount},
    VehicleLightStrip{16, kVehicleLightStripPixelCount},
    AuxiliaryPins{36, 0},
    UsbSerialInterface{"native USB serial", "CH343P"},
    true,
    true,
    false,
};

// Classic ESP32 GPIOs are not a contiguous range. GPIO20, GPIO24, and
// GPIO28-31 do not exist; GPIO34-39 exist but are input-only.
constexpr bool is_valid_gpio(const std::int8_t pin) {
  return (pin >= 0 && pin <= 19) || (pin >= 21 && pin <= 23) || (pin >= 25 && pin <= 27) ||
         (pin >= 32 && pin <= 39);
}

constexpr bool is_output_gpio(const std::int8_t pin) { return is_valid_gpio(pin) && pin <= 33; }

constexpr bool pins_are_distinct(const Capabilities &capabilities) {
  const std::int8_t pins[] = {
      capabilities.can.tx,
      capabilities.can.rx,
      capabilities.rs485.driver_enable,
      capabilities.rs485.receiver_output,
      capabilities.rs485.driver_input,
      capabilities.micro_sd.miso,
      capabilities.micro_sd.mosi,
      capabilities.micro_sd.sclk,
      capabilities.micro_sd.cs,
      capabilities.onboard_rgb.data,
      capabilities.vehicle_light_strip.data,
      capabilities.auxiliary.vin_sense,
      capabilities.auxiliary.user_key,
  };
  for (std::size_t first = 0; first < sizeof(pins) / sizeof(pins[0]); ++first) {
    for (std::size_t second = first + 1; second < sizeof(pins) / sizeof(pins[0]); ++second) {
      if (pins[first] == pins[second]) {
        return false;
      }
    }
  }
  return true;
}

constexpr bool is_configuration_valid(const Capabilities &capabilities = kWeActCan485V11) {
  return is_output_gpio(capabilities.can.tx) && is_valid_gpio(capabilities.can.rx) &&
         is_output_gpio(capabilities.rs485.driver_enable) &&
         is_valid_gpio(capabilities.rs485.receiver_output) &&
         is_output_gpio(capabilities.rs485.driver_input) &&
         is_valid_gpio(capabilities.micro_sd.miso) && is_output_gpio(capabilities.micro_sd.mosi) &&
         is_output_gpio(capabilities.micro_sd.sclk) && is_output_gpio(capabilities.micro_sd.cs) &&
         is_output_gpio(capabilities.onboard_rgb.data) &&
         capabilities.onboard_rgb.pixel_count == kOnboardRgbPixelCount &&
         is_output_gpio(capabilities.vehicle_light_strip.data) &&
         capabilities.vehicle_light_strip.pixel_count == kVehicleLightStripPixelCount &&
         is_valid_gpio(capabilities.auxiliary.vin_sense) &&
         !is_output_gpio(capabilities.auxiliary.vin_sense) &&
         is_valid_gpio(capabilities.auxiliary.user_key) && pins_are_distinct(capabilities) &&
         capabilities.can_transceiver_always_powered && capabilities.vehicle_listen_only &&
         !capabilities.can_transmit_api_exposed;
}

static_assert(is_configuration_valid(),
              "WeAct CAN485 DevBoard V1.1 configuration must be safe and complete");

// Apply the electrical safe state before any peripheral driver is started:
// CAN TX is recessive, RS485 driver output is disabled, and both WS2812B data
// lines are held low so they emit no new command pulses. A low data line cannot
// clear a color latched before a warm reset; local_argb sends RMT-encoded black
// frames before CAN starts. No CAN driver is started. Returns false if a GPIO
// call fails.
bool initialize_safe_defaults();

} // namespace board
