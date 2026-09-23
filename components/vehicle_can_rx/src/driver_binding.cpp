#include "can_bus/driver_binding.hpp"

#include "board/board_config.h"

namespace can_bus::internal {

void configure_driver(twai_general_config_t &configuration) noexcept {
  configuration = TWAI_GENERAL_CONFIG_DEFAULT(
      static_cast<gpio_num_t>(board::kWeActCan485V11.can.tx),
      static_cast<gpio_num_t>(board::kWeActCan485V11.can.rx), TWAI_MODE_LISTEN_ONLY);
  // Strict vehicle acquisition never acknowledges or transmits frames.
  configuration.tx_queue_len = 0;
}

} // namespace can_bus::internal
