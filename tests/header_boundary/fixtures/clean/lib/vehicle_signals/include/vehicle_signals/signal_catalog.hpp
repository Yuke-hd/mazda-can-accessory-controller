#pragma once

#include <cstddef>

#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_signals {

class SignalCatalogView final {
public:
  [[nodiscard]] constexpr std::size_t size() const noexcept { return 0; }
};

} // namespace vehicle_signals
