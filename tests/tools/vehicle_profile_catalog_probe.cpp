#include <array>
#include <iostream>
#include <string>
#include <vector>

#include "companion_protocol/signal_catalog_export.hpp"
#include "generated_profile.hpp"

int main(int argc, const char *[]) {
  using namespace vehicle_signals;
  if (argc > 1) {
    std::array<SignalMetadata, 65> entries{};
    std::array<std::string, 65> keys{};
    for (std::size_t i = 0; i < entries.size(); ++i) {
      keys[i] = "signal." + std::to_string(i);
      entries[i] = {SignalId{static_cast<std::uint16_t>(i + 1)},
                    keys[i],
                    SignalType::Boolean,
                    SignalUnit::None,
                    ValidationStatus::Reference,
                    SignalCapability::Read,
                    nullptr,
                    0};
    }
    if (!companion_protocol::SignalCatalogExport({entries.data(), 64}).available() ||
        companion_protocol::SignalCatalogExport({entries.data(), 65}).available())
      return 1;
    keys[0] = std::string(63, 'a');
    entries[0].key = keys[0];
    if (!companion_protocol::SignalCatalogExport({entries.data(), 1}).available())
      return 2;
    keys[0] += 'a';
    entries[0].key = keys[0];
    if (companion_protocol::SignalCatalogExport({entries.data(), 1}).available())
      return 3;
    std::array<SignalEnumChoice, 33> choices{};
    std::array<std::string, 33> choice_keys{};
    for (std::size_t i = 0; i < choices.size(); ++i) {
      choice_keys[i] = "choice." + std::to_string(i);
      choices[i] = {static_cast<std::uint16_t>(i), choice_keys[i]};
    }
    entries[0].key = "enum";
    entries[0].type = SignalType::Enum;
    entries[0].choices = choices.data();
    entries[0].choice_count = 32;
    if (!companion_protocol::SignalCatalogExport({entries.data(), 1}).available())
      return 4;
    entries[0].choice_count = 33;
    if (companion_protocol::SignalCatalogExport({entries.data(), 1}).available())
      return 5;
    entries[0].choice_count = 32;
    for (std::size_t i = 0; i < 32; ++i) {
      choice_keys[i] += std::string(63 - choice_keys[i].size(), 'x');
      choices[i].key = choice_keys[i];
    }
    for (std::size_t i = 1; i < 64; ++i) {
      entries[i].type = SignalType::Enum;
      entries[i].choices = choices.data();
      entries[i].choice_count = 32;
    }
    if (companion_protocol::SignalCatalogExport({entries.data(), 64}).available())
      return 6;
    return 0;
  }
  const auto catalog = vehicle_profile::kCatalog;
  const companion_protocol::SignalCatalogExport exported{catalog};
  if (!catalog.well_formed() || !exported.available())
    return 10;
  std::cout << exported.signal_count() << '\t' << exported.document_length() << '\n';
  for (const auto &signal : catalog) {
    std::cout << signal.id.value() << '\t' << signal.key << '\t' << static_cast<int>(signal.type)
              << '\t' << static_cast<int>(signal.unit) << '\t'
              << static_cast<int>(signal.validation) << '\t'
              << static_cast<int>(signal.capabilities.bits());
    for (std::size_t i = 0; i < signal.choice_count; ++i)
      std::cout << '\t' << signal.choices[i].value << ':' << signal.choices[i].key;
    std::cout << '\n';
  }
}
