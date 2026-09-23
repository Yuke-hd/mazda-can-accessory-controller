#include <cassert>
#include <string_view>

#include "board/board_config.h"

namespace {

constexpr board::Capabilities kDuplicatePinConfiguration{
    board::HardwareRevision::kWeActCan485DevBoardV11,
    board::CanPins{27, 26},
    board::Rs485Pins{17, 21, 22},
    board::MicroSdPins{2, 15, 14, 13},
    board::OnboardRgb{13, board::kOnboardRgbPixelCount},
    board::VehicleLightStrip{16, board::kVehicleLightStripPixelCount},
    board::AuxiliaryPins{36, 0},
    board::UsbSerialInterface{"native USB serial", "CH343P"},
    true,
    true,
    false,
};

constexpr board::Capabilities kTransmitEnabledConfiguration{
    board::HardwareRevision::kWeActCan485DevBoardV11,
    board::CanPins{27, 26},
    board::Rs485Pins{17, 21, 22},
    board::MicroSdPins{2, 15, 14, 13},
    board::OnboardRgb{4, board::kOnboardRgbPixelCount},
    board::VehicleLightStrip{16, board::kVehicleLightStripPixelCount},
    board::AuxiliaryPins{36, 0},
    board::UsbSerialInterface{"native USB serial", "CH343P"},
    true,
    true,
    true,
};

constexpr board::Capabilities kWrongPixelCountConfiguration{
    board::HardwareRevision::kWeActCan485DevBoardV11,
    board::CanPins{27, 26},
    board::Rs485Pins{17, 21, 22},
    board::MicroSdPins{2, 15, 14, 13},
    board::OnboardRgb{4, board::kVehicleLightStripPixelCount},
    board::VehicleLightStrip{16, board::kVehicleLightStripPixelCount},
    board::AuxiliaryPins{36, 0},
    board::UsbSerialInterface{"native USB serial", "CH343P"},
    true,
    true,
    false,
};

constexpr board::Capabilities kControllableTransceiverConfiguration{
    board::HardwareRevision::kWeActCan485DevBoardV11,
    board::CanPins{27, 26},
    board::Rs485Pins{17, 21, 22},
    board::MicroSdPins{2, 15, 14, 13},
    board::OnboardRgb{4, board::kOnboardRgbPixelCount},
    board::VehicleLightStrip{16, board::kVehicleLightStripPixelCount},
    board::AuxiliaryPins{36, 0},
    board::UsbSerialInterface{"native USB serial", "CH343P"},
    false,
    true,
    false,
};

static_assert(board::is_configuration_valid(), "the WeAct V1.1 pin record must be valid");
static_assert(!board::is_valid_gpio(20), "GPIO20 is not present on classic ESP32");
static_assert(!board::is_valid_gpio(24), "GPIO24 is not present on classic ESP32");
static_assert(!board::is_valid_gpio(28), "GPIO28 is not present on classic ESP32");
static_assert(!board::is_valid_gpio(31), "GPIO31 is not present on classic ESP32");
static_assert(board::is_valid_gpio(36), "GPIO36 is a valid input-only GPIO");
static_assert(!board::is_output_gpio(36), "GPIO36 must not be configured as output");
static_assert(board::is_valid_gpio(16), "GPIO16 must be a valid classic ESP32 GPIO");
static_assert(board::is_output_gpio(16), "GPIO16 must support output for the vehicle strip");
static_assert(!board::is_configuration_valid(kDuplicatePinConfiguration),
              "duplicate pins must fail closed at compile time");
static_assert(!board::is_configuration_valid(kTransmitEnabledConfiguration),
              "a CAN transmit API must fail closed at compile time");
static_assert(!board::is_configuration_valid(kWrongPixelCountConfiguration),
              "the onboard WS2812B count must be exactly one");
static_assert(!board::is_configuration_valid(kControllableTransceiverConfiguration),
              "the always-powered CAN transceiver invariant must fail closed");

} // namespace

int main() {
  assert(board::kWeActCan485V11.revision == board::HardwareRevision::kWeActCan485DevBoardV11);
  assert(board::kWeActCan485V11.can.tx == 27);
  assert(board::kWeActCan485V11.can.rx == 26);
  assert(board::kWeActCan485V11.rs485.driver_enable == 17);
  assert(board::kWeActCan485V11.rs485.receiver_output == 21);
  assert(board::kWeActCan485V11.rs485.driver_input == 22);
  assert(board::kWeActCan485V11.micro_sd.miso == 2);
  assert(board::kWeActCan485V11.micro_sd.mosi == 15);
  assert(board::kWeActCan485V11.micro_sd.sclk == 14);
  assert(board::kWeActCan485V11.micro_sd.cs == 13);
  assert(board::kWeActCan485V11.onboard_rgb.data == 4);
  assert(board::kWeActCan485V11.onboard_rgb.pixel_count == board::kOnboardRgbPixelCount);
  assert(board::kWeActCan485V11.vehicle_light_strip.data == 16);
  assert(board::kWeActCan485V11.vehicle_light_strip.pixel_count ==
         board::kVehicleLightStripPixelCount);
  assert(board::kWeActCan485V11.auxiliary.vin_sense == 36);
  assert(board::kWeActCan485V11.auxiliary.user_key == 0);
  assert(std::string_view{board::kWeActCan485V11.usb_serial.transport} == "native USB serial");
  assert(std::string_view{board::kWeActCan485V11.usb_serial.bridge} == "CH343P");
  assert(board::kWeActCan485V11.can_transceiver_always_powered);
  assert(board::kWeActCan485V11.vehicle_listen_only);
  assert(!board::kWeActCan485V11.can_transmit_api_exposed);
  return 0;
}
