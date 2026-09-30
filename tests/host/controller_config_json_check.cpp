#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <type_traits>
#include <variant>

#include "action_engine/condition.hpp"
#include "action_engine/range_rule.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "mazda/signal_catalog.hpp"

namespace persisted = controller_config::persisted;

namespace {
void print_catalog() {
  std::cout << '{';
  bool first = true;
  for (const auto &signal : mazda::internal::signal_catalog()) {
    if (!first)
      std::cout << ',';
    first = false;
    const auto type = signal.type == vehicle_signals::SignalType::Boolean  ? "boolean"
                      : signal.type == vehicle_signals::SignalType::Number ? "number"
                                                                           : "enum";
    std::cout << std::quoted(std::string(signal.key)) << ":{\"type\":" << std::quoted(type)
              << ",\"capabilities\":[";
    const bool read = signal.capabilities.has(vehicle_signals::SignalCapability::Read);
    if (read)
      std::cout << "\"read\"";
    if (signal.capabilities.has(vehicle_signals::SignalCapability::Notify))
      std::cout << (read ? ",\"notify\"" : "\"notify\"");
    std::cout << ']';
    if (signal.type == vehicle_signals::SignalType::Enum) {
      std::cout << ",\"choices\":[";
      for (std::size_t index = 0; index < signal.choice_count; ++index) {
        if (index != 0)
          std::cout << ',';
        std::cout << std::quoted(std::string(signal.choices[index].key));
      }
      std::cout << ']';
    }
    std::cout << '}';
  }
  std::cout << "}\n";
}

[[nodiscard]] action_engine::RuleOperand operand(const persisted::Operand &value) {
  return std::visit(
      [](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, persisted::BooleanOperand>)
          return action_engine::RuleOperand::boolean(item.value);
        else if constexpr (std::is_same_v<T, persisted::NumberOperand>)
          return action_engine::RuleOperand::number(item.value);
        else
          return action_engine::RuleOperand::choice(item.key);
      },
      value);
}

[[nodiscard]] action_engine::ConfigStatus resolve(const persisted::Rule &rule) {
  const auto catalog = mazda::internal::signal_catalog();
  return std::visit(
      [&](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, persisted::RangeRule>) {
          return action_engine::resolve_range_rule(catalog,
                                                   {item.signal_key, item.input, item.output,
                                                    action_engine::ActionId{1}, item.freshness})
              .status;
        } else {
          const action_engine::SignalCondition condition{item.condition.signal_key,
                                                         item.condition.comparison,
                                                         operand(item.condition.operand)};
          if constexpr (std::is_same_v<T, persisted::SampledStateRule>)
            return action_engine::resolve_sampled_condition(catalog, condition, item.freshness)
                .status;
          else
            return action_engine::resolve_condition(catalog, condition, item.freshness).status;
        }
      },
      rule);
}
} // namespace

int main(const int argc, const char *const argv[]) {
  if (argc != 2)
    return 2;
  if (std::string_view(argv[1]) == "--catalog") {
    print_catalog();
    return 0;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input)
    return 2;
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto loaded = persisted::parse_controller_config(json);
  if (!loaded.ok()) {
    std::cerr << loaded.diagnostic->path << ": " << loaded.diagnostic->message << '\n';
    return 1;
  }
  for (std::size_t index = 0; index < loaded.configuration->rules.size(); ++index) {
    if (const auto status = resolve(loaded.configuration->rules[index]);
        status != action_engine::ConfigStatus::Ok) {
      std::cerr << "rules[" << index << "]: catalog resolution failed ("
                << static_cast<unsigned>(status) << ")\n";
      return 1;
    }
  }
  return 0;
}
