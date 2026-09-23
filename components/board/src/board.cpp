#include "board/board_config.h"

#include "driver/gpio.h"

namespace board {
namespace {

esp_err_t configure_output(const std::int8_t pin, const int level) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  esp_err_t result = gpio_reset_pin(gpio);
  if (result != ESP_OK) {
    return result;
  }

  // Set the output latch before enabling output so the transition cannot drive
  // a dominant CAN bit, enable RS485, or create a new LED command pulse.
  result = gpio_set_level(gpio, level);
  if (result != ESP_OK) {
    return result;
  }
  return gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
}

esp_err_t configure_input(const std::int8_t pin) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  esp_err_t result = gpio_reset_pin(gpio);
  if (result != ESP_OK) {
    return result;
  }
  return gpio_set_direction(gpio, GPIO_MODE_INPUT);
}

} // namespace

bool initialize_safe_defaults() {
  if (!is_configuration_valid()) {
    return false;
  }

  // The CA-IS2062A has no software-controlled supply or standby line. Put the
  // controller TX latch in its recessive state before TWAI takes ownership.
  bool ok = true;
  ok = configure_output(kWeActCan485V11.onboard_rgb.data, 0) == ESP_OK && ok;
  ok = configure_output(kWeActCan485V11.vehicle_light_strip.data, 0) == ESP_OK && ok;
  ok = configure_output(kWeActCan485V11.can.tx, 1) == ESP_OK && ok;
  ok = configure_output(kWeActCan485V11.rs485.driver_enable, 0) == ESP_OK && ok;
  ok = configure_output(kWeActCan485V11.rs485.driver_input, 1) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.can.rx) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.rs485.receiver_output) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.micro_sd.miso) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.micro_sd.mosi) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.micro_sd.sclk) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.micro_sd.cs) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.auxiliary.vin_sense) == ESP_OK && ok;
  ok = configure_input(kWeActCan485V11.auxiliary.user_key) == ESP_OK && ok;

  // Best-effort reassertion if a later GPIO operation failed. The caller must
  // refuse to start the application when ok is false.
  if (!ok) {
    (void)configure_output(kWeActCan485V11.onboard_rgb.data, 0);
    (void)configure_output(kWeActCan485V11.vehicle_light_strip.data, 0);
    (void)configure_output(kWeActCan485V11.can.tx, 1);
    (void)configure_output(kWeActCan485V11.rs485.driver_enable, 0);
  }
  return ok;
}

} // namespace board
