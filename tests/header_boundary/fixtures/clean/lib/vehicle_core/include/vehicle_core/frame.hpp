#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vehicle_core/time.hpp"

namespace vehicle_core {

constexpr std::size_t kCanClassicPayloadBytes = 8;

struct RawCanFrame {
  MonotonicTimestamp timestamp_us{0};
  std::uint8_t bus_id{0};
  std::uint32_t identifier{0};
  std::array<std::uint8_t, kCanClassicPayloadBytes> data{};
};

} // namespace vehicle_core
