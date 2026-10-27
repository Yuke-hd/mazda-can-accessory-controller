#pragma once

#include <iomanip>
#include <ostream>
#include <string>

#include "vehicle_signals/signal_catalog.hpp"

namespace controller_signal_catalog {

// Compiler-facing catalog projection. Runtime ids, source extraction and
// freshness policy are outside this existing compatibility representation.
inline void write(std::ostream &output, vehicle_signals::SignalCatalogView catalog) {
  output << '{';
  bool first = true;
  for (const auto &signal : catalog) {
    if (!first)
      output << ',';
    first = false;
    const auto type = signal.type == vehicle_signals::SignalType::Boolean  ? "boolean"
                      : signal.type == vehicle_signals::SignalType::Number ? "number"
                                                                           : "enum";
    output << std::quoted(std::string(signal.key)) << ":{\"type\":" << std::quoted(type)
           << ",\"capabilities\":[";
    const bool read = signal.capabilities.has(vehicle_signals::SignalCapability::Read);
    if (read)
      output << "\"read\"";
    if (signal.capabilities.has(vehicle_signals::SignalCapability::Notify))
      output << (read ? ",\"notify\"" : "\"notify\"");
    output << ']';
    if (signal.type == vehicle_signals::SignalType::Enum) {
      output << ",\"choices\":[";
      for (std::size_t index = 0; index < signal.choice_count; ++index) {
        if (index != 0)
          output << ',';
        output << std::quoted(std::string(signal.choices[index].key));
      }
      output << ']';
    }
    output << '}';
  }
  output << "}\n";
}

} // namespace controller_signal_catalog
