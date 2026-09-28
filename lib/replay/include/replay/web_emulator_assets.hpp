#pragma once

#include <string_view>

namespace replay {

// One browser asset compiled into the emulator. The sources live in
// lib/replay/web/ and are embedded at configure time, so the executable serves
// them without reading files at run time.
struct WebEmulatorAsset final {
  std::string_view content_type;
  std::string_view body;
};

// Returns the embedded asset served at an HTTP request target, or nullptr when
// no asset exists at that path.
[[nodiscard]] const WebEmulatorAsset *find_web_emulator_asset(std::string_view target) noexcept;

} // namespace replay
