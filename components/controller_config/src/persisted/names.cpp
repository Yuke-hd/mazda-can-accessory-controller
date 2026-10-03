#include "controller_config/persisted/names.hpp"

#include <array>
#include <utility>

namespace controller_config::persisted {
namespace {

using action_engine::Comparison;
using action_engine::EventEdge;
using action_engine::FreshnessRequirement;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;

template <typename Enum, std::size_t Count>
using NameTable = std::array<std::pair<Enum, std::string_view>, Count>;

// One table per persisted enum. Adding an enumerator to a runtime enum does
// not make it persistable: it must be given a name here, deliberately.
template <typename Enum> struct Names;

template <> struct Names<RuleType> {
  static constexpr NameTable<RuleType, 4> kTable{{{RuleType::State, "state"},
                                                  {RuleType::SampledState, "sampled_state"},
                                                  {RuleType::Event, "event"},
                                                  {RuleType::Range, "range"}}};
};

template <> struct Names<OutputType> {
  static constexpr NameTable<OutputType, 4> kTable{{{OutputType::LedEffect, "led_effect"},
                                                    {OutputType::LedFill, "led_fill"},
                                                    {OutputType::LedTransient, "led_transient"},
                                                    {OutputType::LedSolid, "led_solid"}}};
};

template <> struct Names<Comparison> {
  static constexpr NameTable<Comparison, 6> kTable{
      {{Comparison::Equal, "equal"},
       {Comparison::NotEqual, "not_equal"},
       {Comparison::Less, "less"},
       {Comparison::LessOrEqual, "less_or_equal"},
       {Comparison::Greater, "greater"},
       {Comparison::GreaterOrEqual, "greater_or_equal"}}};
};

template <> struct Names<FreshnessRequirement> {
  static constexpr NameTable<FreshnessRequirement, 2> kTable{
      {{FreshnessRequirement::Fresh, "fresh"},
       {FreshnessRequirement::FreshOrUnverified, "fresh_or_unverified"}}};
};

template <> struct Names<EventEdge> {
  static constexpr NameTable<EventEdge, 2> kTable{
      {{EventEdge::BecomesTrue, "becomes_true"}, {EventEdge::BecomesFalse, "becomes_false"}}};
};

template <> struct Names<LedEffect> {
  static constexpr NameTable<LedEffect, 3> kTable{{{LedEffect::LeftTurn, "left_turn"},
                                                   {LedEffect::RightTurn, "right_turn"},
                                                   {LedEffect::Brake, "brake"}}};
};

template <> struct Names<FillDirection> {
  static constexpr NameTable<FillDirection, 3> kTable{{{FillDirection::StartToEnd, "start_to_end"},
                                                       {FillDirection::EndToStart, "end_to_start"},
                                                       {FillDirection::CenterOut, "center_out"}}};
};

template <typename Enum> std::optional<std::string_view> lookup_name(const Enum value) noexcept {
  for (const auto &[entry, name] : Names<Enum>::kTable) {
    if (entry == value)
      return name;
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string_view> name_of(const RuleType value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const OutputType value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const Comparison value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const FreshnessRequirement value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const EventEdge value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const LedEffect value) noexcept {
  return lookup_name(value);
}
std::optional<std::string_view> name_of(const FillDirection value) noexcept {
  return lookup_name(value);
}

template <typename Enum> std::optional<Enum> parse_name(const std::string_view name) noexcept {
  for (const auto &[value, entry] : Names<Enum>::kTable) {
    if (entry == name)
      return value;
  }
  return std::nullopt;
}

template std::optional<RuleType> parse_name<RuleType>(std::string_view) noexcept;
template std::optional<OutputType> parse_name<OutputType>(std::string_view) noexcept;
template std::optional<Comparison> parse_name<Comparison>(std::string_view) noexcept;
template std::optional<FreshnessRequirement>
    parse_name<FreshnessRequirement>(std::string_view) noexcept;
template std::optional<EventEdge> parse_name<EventEdge>(std::string_view) noexcept;
template std::optional<LedEffect> parse_name<LedEffect>(std::string_view) noexcept;
template std::optional<FillDirection> parse_name<FillDirection>(std::string_view) noexcept;

} // namespace controller_config::persisted
