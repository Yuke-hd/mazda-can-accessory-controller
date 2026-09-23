#include <cstddef>
#include <iomanip>
#include <iostream>

#include "mazda/definitions.hpp"

namespace {

const char *confidence_name(const vehicle_core::ValidationStatus confidence) noexcept {
  switch (confidence) {
  case vehicle_core::ValidationStatus::Reference:
    return "Reference";
  case vehicle_core::ValidationStatus::Observed:
    return "Observed";
  case vehicle_core::ValidationStatus::Confirmed:
    return "Confirmed";
  }
  return "Invalid";
}

const char *
byte_order_name(const mazda::candidate::CandidateSignalDefinition::ByteOrder byte_order) noexcept {
  return byte_order == mazda::candidate::CandidateSignalDefinition::ByteOrder::Intel ? "Intel"
                                                                                     : "Motorola";
}

const char *unit_name(const vehicle_core::SignalUnit unit) noexcept {
  switch (unit) {
  case vehicle_core::SignalUnit::None:
    return "None";
  case vehicle_core::SignalUnit::KilometresPerHour:
    return "KilometresPerHour";
  case vehicle_core::SignalUnit::RevolutionsPerMinute:
    return "RevolutionsPerMinute";
  case vehicle_core::SignalUnit::Boolean:
    return "Boolean";
  }
  return "Invalid";
}

} // namespace

int main() {
  using mazda::candidate::kSupportedSignalDefinitions;

  // This intentionally stable, tab-delimited format is a host-tool seam, not
  // a runtime schema. It exports the compiled constexpr table so comparison
  // cannot accidentally validate a duplicated Python/C++ literal.
  std::cout << "mazda-metadata-v1\n";
  std::cout << "message\tdbc_signal\tchannel\tname\tidentifier\tstart_bit\tdbc_start_bit\tbit_length"
               "\tscale\toffset\tphysical_min\tphysical_max\tbyte_order\tunit\tconfidence"
               "\tprovenance\n";
  std::cout << std::setprecision(9);
  for (const auto &entry : kSupportedSignalDefinitions) {
    const auto &definition = *entry.definition;
    std::cout << entry.message_name << '\t' << entry.dbc_signal_name << '\t' << entry.channel_name
              << '\t' << definition.name << '\t' << definition.identifier << '\t'
              << static_cast<unsigned>(definition.start_bit) << '\t'
              << static_cast<unsigned>(definition.dbc_start_bit) << '\t'
              << static_cast<unsigned>(definition.bit_length) << '\t' << definition.scale << '\t'
              << definition.offset << '\t' << definition.physical_min << '\t'
              << definition.physical_max << '\t' << byte_order_name(definition.byte_order) << '\t'
              << unit_name(definition.unit) << '\t' << confidence_name(definition.confidence)
              << '\t' << (definition.provenance == nullptr ? "" : definition.provenance) << '\n';
  }
  return 0;
}
