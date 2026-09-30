#pragma once

#include <string_view>

namespace controller_config {

// Returns the canonical factory configuration embedded by the firmware build.
// The returned view is read-only and remains valid for the lifetime of the
// firmware image.
[[nodiscard]] std::string_view factory_default_config_json() noexcept;

} // namespace controller_config
