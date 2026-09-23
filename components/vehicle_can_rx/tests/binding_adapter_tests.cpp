#include "can_bus/driver_binding.hpp"

#include <cassert>

int main() {
  twai_general_config_t configuration{};
  can_bus::internal::configure_driver(configuration);
  assert(configuration.tx_io == 27);
  assert(configuration.rx_io == 26);
  assert(configuration.mode == TWAI_MODE_LISTEN_ONLY);
  assert(configuration.tx_queue_len == 0);
  return 0;
}
