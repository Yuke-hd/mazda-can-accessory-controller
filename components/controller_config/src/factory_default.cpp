#include "controller_config/factory_default.hpp"

#include <cstddef>

namespace controller_config {

#if defined(CONTROLLER_CONFIG_FACTORY_DEFAULT_EMBEDDED)
extern "C" {
extern const unsigned char _binary_factory_default_config_json_start[];
extern const unsigned char _binary_factory_default_config_json_end[];
}
#endif

std::string_view factory_default_config_json() noexcept {
#if defined(CONTROLLER_CONFIG_FACTORY_DEFAULT_EMBEDDED)
  const auto *const begin =
      reinterpret_cast<const char *>(_binary_factory_default_config_json_start);
  const auto *const end = reinterpret_cast<const char *>(_binary_factory_default_config_json_end);
  return std::string_view{begin, static_cast<std::size_t>(end - begin)};
#else
  return {};
#endif
}

} // namespace controller_config
