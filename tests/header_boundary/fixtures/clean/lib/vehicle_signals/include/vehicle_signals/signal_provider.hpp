#pragma once

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_signals {

class SignalProvider {
public:
  [[nodiscard]] virtual SignalCatalogView catalog() const noexcept = 0;

protected:
  ~SignalProvider() = default;
};

} // namespace vehicle_signals
