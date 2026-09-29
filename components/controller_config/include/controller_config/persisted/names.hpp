#pragma once

#include <optional>
#include <string_view>

#include "action_engine/rule_config.hpp"
#include "controller_config/persisted/model.hpp"
#include "local_argb/lighting_zone.hpp"
#include "local_argb_actions/effect_bindings.hpp"

// Persisted spellings of the version 1 schema's enum-like values. These tables
// are the single source of truth for which values a document may use: a
// loader maps a persisted name with parse_name(), and validate() treats an
// enumerator without a name_of() spelling as unrecognized.

namespace controller_config::persisted {

// The persisted name of `value`, or std::nullopt when `value` is not a
// recognized enumerator (for example an out-of-range cast).
[[nodiscard]] std::optional<std::string_view> name_of(RuleType value) noexcept;
[[nodiscard]] std::optional<std::string_view> name_of(OutputType value) noexcept;
[[nodiscard]] std::optional<std::string_view> name_of(action_engine::Comparison value) noexcept;
[[nodiscard]] std::optional<std::string_view>
name_of(action_engine::FreshnessRequirement value) noexcept;
[[nodiscard]] std::optional<std::string_view> name_of(action_engine::EventEdge value) noexcept;
[[nodiscard]] std::optional<std::string_view> name_of(local_argb_actions::LedEffect value) noexcept;
[[nodiscard]] std::optional<std::string_view>
name_of(local_argb::internal::FillDirection value) noexcept;

// The enumerator persisted as `name`, or std::nullopt for an unrecognized
// name. Names are case-sensitive. Defined for exactly the enums name_of()
// accepts.
template <typename Enum>
[[nodiscard]] std::optional<Enum> parse_name(std::string_view name) noexcept;

} // namespace controller_config::persisted
