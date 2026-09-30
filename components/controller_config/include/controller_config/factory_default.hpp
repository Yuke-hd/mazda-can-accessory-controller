#pragma once

#include <string_view>

namespace controller_config {

// Returns the canonical factory configuration embedded by the firmware build.
// The returned view is read-only and remains valid for the lifetime of the
// firmware image. ESP-IDF's EMBED_TXTFILES appends one trailing NUL, included
// in this view and accepted by parse_controller_config(). Host builds return
// an empty view because they have no embedded image.
[[nodiscard]] std::string_view factory_default_config_json() noexcept;

} // namespace controller_config
