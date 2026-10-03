#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "controller_config/lighting_profile.hpp"
#include "controller_config/persisted/model.hpp"
#include "controller_config/persisted/names.hpp"
#include "controller_config/persisted/production_profile.hpp"
#include "controller_config/persisted/validation.hpp"
#include "controller_config/rpm_level_fill.hpp"
#include "controller_config/rpm_threshold.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace persisted = controller_config::persisted;

using action_engine::Comparison;
using action_engine::EventEdge;
using action_engine::FreshnessRequirement;
using action_engine::NumericRange;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;
using persisted::ConfigSection;
using persisted::ValidationError;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

persisted::ControllerConfig with_actions(std::initializer_list<const char *> names) {
  persisted::ControllerConfig config{};
  for (const char *name : names)
    config.actions.push_back(persisted::Action{name});
  return config;
}

persisted::StateRule state_rule(std::string action, persisted::Operand operand,
                                Comparison comparison = Comparison::Equal) {
  return persisted::StateRule{
      std::move(action), persisted::Condition{"vehicle.turn_state", comparison, std::move(operand)},
      FreshnessRequirement::Fresh};
}

persisted::SampledStateRule rpm_above(std::string action, float rpm,
                                      std::optional<float> release_threshold = std::nullopt) {
  return persisted::SampledStateRule{std::move(action),
                                     persisted::Condition{"vehicle.engine_rpm", Comparison::Greater,
                                                          persisted::NumberOperand{rpm}},
                                     FreshnessRequirement::FreshOrUnverified, release_threshold};
}

persisted::RangeRule rpm_range(std::string action, NumericRange input,
                               NumericRange output = NumericRange{0.0F, 1.0F}) {
  return persisted::RangeRule{std::move(action), "vehicle.engine_rpm", input, output,
                              FreshnessRequirement::FreshOrUnverified};
}

persisted::LedFillBinding fill(std::string action, persisted::LedZone zone) {
  return persisted::LedFillBinding{std::move(action), zone, persisted::Rgb{0, 16, 32}, 50};
}

void check_error(const persisted::ControllerConfig &config, ValidationError error,
                 ConfigSection section, std::size_t index) {
  const persisted::ValidationResult result = persisted::validate(config);
  CHECK_FALSE(result.ok());
  CHECK(result.error == error);
  CHECK(result.section == section);
  CHECK(result.index == index);
}

template <typename T> T enum_value(std::uint8_t raw) { return static_cast<T>(raw); }

// Looks up a production rule or output by action name so the tests do not
// depend on declaration order or on how many entries the profile holds.
template <typename T, typename Variant>
const T *find_by_action(const std::vector<Variant> &entries, std::string_view action) {
  for (const Variant &entry : entries) {
    const auto *candidate = std::get_if<T>(&entry);
    if (candidate != nullptr && candidate->action == action) {
      return candidate;
    }
  }
  return nullptr;
}

// --- Production example --------------------------------------------------

TEST_CASE("the production example is a valid version 1 configuration") {
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  CHECK(config.version == persisted::kSchemaVersion);
  CHECK(persisted::validate(config).ok());
}

TEST_CASE("the production example names one action per behaviour") {
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  REQUIRE(config.actions.size() == 6);
  CHECK(config.actions[0].name == "left_turn");
  CHECK(config.actions[1].name == "right_turn");
  CHECK(config.actions[2].name == "hazard");
  CHECK(config.actions[3].name == "rpm_fill");
  CHECK(config.actions[4].name == "red_zone");
  CHECK(config.actions[5].name == "brake");
}

TEST_CASE("the production example turn rules match the default lighting profile") {
  const auto &profile = controller_config::kDefaultLightingProfile;
  const persisted::ControllerConfig config = persisted::production_lighting_config();
  const std::pair<std::string_view, std::string_view> expected[] = {
      {"left_turn", profile.turn_left.choice},
      {"right_turn", profile.turn_right.choice},
      {"hazard", profile.hazard.choice}};

  for (std::size_t index = 0; index < 3; ++index) {
    CAPTURE(index);
    const auto *rule = find_by_action<persisted::StateRule>(config.rules, expected[index].first);
    REQUIRE(rule != nullptr);
    CHECK(rule->condition.signal_key == profile.turn_state_signal);
    CHECK(rule->condition.comparison == Comparison::Equal);
    const auto *choice = std::get_if<persisted::ChoiceOperand>(&rule->condition.operand);
    REQUIRE(choice != nullptr);
    CHECK(choice->key == expected[index].second);
    CHECK(rule->freshness == FreshnessRequirement::Fresh);
  }
}

TEST_CASE("the production example RPM fill rule matches the default lighting profile") {
  const action_engine::RangeRuleConfig expected =
      controller_config::range_rule(controller_config::kDefaultLightingProfile.rpm_level_fill);
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  const auto *rule = find_by_action<persisted::RangeRule>(config.rules, "rpm_fill");
  REQUIRE(rule != nullptr);
  CHECK(rule->signal_key == expected.signal_key);
  CHECK(rule->input.from == expected.input.from);
  CHECK(rule->input.to == expected.input.to);
  CHECK(rule->output.from == expected.output.from);
  CHECK(rule->output.to == expected.output.to);
  CHECK(rule->freshness == expected.freshness);
}

TEST_CASE("the production example red zone rule matches the default lighting profile") {
  const action_engine::SampledStateRuleConfig expected =
      controller_config::threshold_rule(controller_config::kDefaultLightingProfile.rpm_red_zone);
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  const auto *rule = find_by_action<persisted::SampledStateRule>(config.rules, "red_zone");
  REQUIRE(rule != nullptr);
  CHECK(rule->condition.signal_key == expected.condition.signal_key);
  CHECK(rule->condition.comparison == expected.condition.comparison);
  const auto *number = std::get_if<persisted::NumberOperand>(&rule->condition.operand);
  REQUIRE(number != nullptr);
  CHECK(expected.condition.operand.as_number() == number->value);
  CHECK(rule->freshness == expected.freshness);
  CHECK(rule->release_threshold == expected.release_threshold);
}

TEST_CASE("the production example turn outputs keep the mirrored strip mapping") {
  const auto &bindings = controller_config::kDefaultLightingProfile.turn_effect_bindings;
  const std::string_view action_names[] = {"left_turn", "right_turn", "hazard", "hazard"};
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  REQUIRE(config.outputs.size() >= bindings.size());
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    CAPTURE(index);
    const auto *binding = std::get_if<persisted::LedEffectBinding>(&config.outputs[index]);
    REQUIRE(binding != nullptr);
    CHECK(binding->action == action_names[index]);
    CHECK(binding->effect == bindings[index].effect);
    CHECK(binding->priority == bindings[index].priority.rank());
  }
}

TEST_CASE("the production example RPM outputs match the default lighting profile") {
  const auto &profile = controller_config::kDefaultLightingProfile;
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  const auto *fill_binding = find_by_action<persisted::LedFillBinding>(config.outputs, "rpm_fill");
  REQUIRE(fill_binding != nullptr);
  const local_argb_actions::FillEffect &expected_fill = profile.rpm_level_fill.fill;
  CHECK(fill_binding->zone.start == static_cast<persisted::Integer>(expected_fill.zone.start));
  CHECK(fill_binding->zone.length == static_cast<persisted::Integer>(expected_fill.zone.length));
  CHECK(fill_binding->zone.direction == expected_fill.zone.direction);
  CHECK(fill_binding->color.red == expected_fill.color.red);
  CHECK(fill_binding->color.green == expected_fill.color.green);
  CHECK(fill_binding->color.blue == expected_fill.color.blue);
  CHECK(fill_binding->priority == expected_fill.priority.rank());

  const auto *red_zone = find_by_action<persisted::LedEffectBinding>(config.outputs, "red_zone");
  REQUIRE(red_zone != nullptr);
  CHECK(red_zone->effect == profile.rpm_red_zone.effect);
  CHECK(red_zone->priority == profile.rpm_red_zone.priority.rank());
}

TEST_CASE("the production example holds the brake action while the pedal is pressed") {
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  const auto *rule = find_by_action<persisted::StateRule>(config.rules, "brake");
  REQUIRE(rule != nullptr);
  CHECK(rule->condition.signal_key == "vehicle.brake_pressed");
  CHECK(rule->condition.comparison == Comparison::Equal);
  const auto *pressed = std::get_if<persisted::BooleanOperand>(&rule->condition.operand);
  REQUIRE(pressed != nullptr);
  CHECK(pressed->value);
  // Brake has no evidence-backed freshness timeout; the owner-approved
  // opt-in accepts an unverified observation without promoting it to Fresh.
  CHECK(rule->freshness == FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("the production example binds the brake action above the RPM red zone") {
  const persisted::ControllerConfig config = persisted::production_lighting_config();

  const auto *brake = find_by_action<persisted::LedEffectBinding>(config.outputs, "brake");
  REQUIRE(brake != nullptr);
  CHECK(brake->effect == LedEffect::Brake);
  CHECK(brake->priority == 200);
  const auto *red_zone = find_by_action<persisted::LedEffectBinding>(config.outputs, "red_zone");
  REQUIRE(red_zone != nullptr);
  CHECK(red_zone->priority < brake->priority);
}

// --- Defaults --------------------------------------------------------------

TEST_CASE("omitted optional fields take the documented defaults") {
  CHECK(persisted::ControllerConfig{}.version == 1);
  CHECK(persisted::StateRule{}.freshness == FreshnessRequirement::Fresh);
  CHECK(persisted::SampledStateRule{}.freshness == FreshnessRequirement::Fresh);
  CHECK_FALSE(persisted::SampledStateRule{}.release_threshold.has_value());
  CHECK(persisted::EventRule{}.freshness == FreshnessRequirement::Fresh);
  CHECK(persisted::RangeRule{}.freshness == FreshnessRequirement::Fresh);
  CHECK(persisted::LedEffectBinding{}.priority == 100);
  CHECK(persisted::LedFillBinding{}.priority == 100);
}

// --- Persisted names -------------------------------------------------------

TEST_CASE("every enum value has one persisted name that parses back to it") {
  CHECK(persisted::name_of(persisted::RuleType::State) == std::string_view{"state"});
  CHECK(persisted::name_of(persisted::RuleType::SampledState) == std::string_view{"sampled_state"});
  CHECK(persisted::name_of(persisted::RuleType::Event) == std::string_view{"event"});
  CHECK(persisted::name_of(persisted::RuleType::Range) == std::string_view{"range"});
  CHECK(persisted::name_of(persisted::OutputType::LedEffect) == std::string_view{"led_effect"});
  CHECK(persisted::name_of(persisted::OutputType::LedFill) == std::string_view{"led_fill"});
  CHECK(persisted::name_of(Comparison::Equal) == std::string_view{"equal"});
  CHECK(persisted::name_of(Comparison::NotEqual) == std::string_view{"not_equal"});
  CHECK(persisted::name_of(Comparison::Less) == std::string_view{"less"});
  CHECK(persisted::name_of(Comparison::LessOrEqual) == std::string_view{"less_or_equal"});
  CHECK(persisted::name_of(Comparison::Greater) == std::string_view{"greater"});
  CHECK(persisted::name_of(Comparison::GreaterOrEqual) == std::string_view{"greater_or_equal"});
  CHECK(persisted::name_of(FreshnessRequirement::Fresh) == std::string_view{"fresh"});
  CHECK(persisted::name_of(FreshnessRequirement::FreshOrUnverified) ==
        std::string_view{"fresh_or_unverified"});
  CHECK(persisted::name_of(EventEdge::BecomesTrue) == std::string_view{"becomes_true"});
  CHECK(persisted::name_of(EventEdge::BecomesFalse) == std::string_view{"becomes_false"});
  CHECK(persisted::name_of(LedEffect::LeftTurn) == std::string_view{"left_turn"});
  CHECK(persisted::name_of(LedEffect::RightTurn) == std::string_view{"right_turn"});
  CHECK(persisted::name_of(LedEffect::Brake) == std::string_view{"brake"});
  CHECK(persisted::name_of(FillDirection::StartToEnd) == std::string_view{"start_to_end"});
  CHECK(persisted::name_of(FillDirection::EndToStart) == std::string_view{"end_to_start"});
  CHECK(persisted::name_of(FillDirection::CenterOut) == std::string_view{"center_out"});

  CHECK(persisted::parse_name<persisted::RuleType>("sampled_state") ==
        persisted::RuleType::SampledState);
  CHECK(persisted::parse_name<persisted::OutputType>("led_fill") == persisted::OutputType::LedFill);
  CHECK(persisted::parse_name<Comparison>("greater_or_equal") == Comparison::GreaterOrEqual);
  CHECK(persisted::parse_name<FreshnessRequirement>("fresh_or_unverified") ==
        FreshnessRequirement::FreshOrUnverified);
  CHECK(persisted::parse_name<EventEdge>("becomes_false") == EventEdge::BecomesFalse);
  CHECK(persisted::parse_name<LedEffect>("brake") == LedEffect::Brake);
  CHECK(persisted::parse_name<FillDirection>("center_out") == FillDirection::CenterOut);
}

TEST_CASE("unrecognized persisted names and enum values are rejected") {
  CHECK_FALSE(persisted::parse_name<Comparison>("Equal").has_value());
  CHECK_FALSE(persisted::parse_name<Comparison>("==").has_value());
  CHECK_FALSE(persisted::parse_name<LedEffect>("").has_value());
  CHECK_FALSE(persisted::parse_name<FillDirection>("centre_out").has_value());
  CHECK_FALSE(persisted::parse_name<persisted::RuleType>("threshold").has_value());

  CHECK_FALSE(persisted::name_of(enum_value<Comparison>(99)).has_value());
  CHECK_FALSE(persisted::name_of(enum_value<LedEffect>(99)).has_value());
  CHECK_FALSE(persisted::name_of(enum_value<FillDirection>(99)).has_value());
}

// --- Document --------------------------------------------------------------

TEST_CASE("an empty version 1 document is valid") {
  CHECK(persisted::validate(persisted::ControllerConfig{}).ok());
}

TEST_CASE("any version other than 1 is unsupported") {
  for (const persisted::Integer version :
       {persisted::Integer{0}, persisted::Integer{2}, persisted::Integer{-1}}) {
    CAPTURE(version);
    persisted::ControllerConfig config = persisted::production_lighting_config();
    config.version = version;
    check_error(config, ValidationError::UnsupportedVersion, ConfigSection::Document, 0);
  }
}

// --- Actions ---------------------------------------------------------------

TEST_CASE("action names must be non-empty and unique") {
  check_error(with_actions({"left_turn", ""}), ValidationError::EmptyActionName,
              ConfigSection::Actions, 1);
  check_error(with_actions({"left_turn", "hazard", "left_turn"}),
              ValidationError::DuplicateActionName, ConfigSection::Actions, 2);
}

TEST_CASE("an action with no rule or output is valid") {
  CHECK(persisted::validate(with_actions({"unused"})).ok());
}

// --- Rules -----------------------------------------------------------------

TEST_CASE("a rule must name a declared action") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  config.rules.push_back(state_rule("left_turn", persisted::ChoiceOperand{"left"}));
  config.rules.push_back(state_rule("right_turn", persisted::ChoiceOperand{"right"}));

  check_error(config, ValidationError::UndeclaredAction, ConfigSection::Rules, 1);
}

TEST_CASE("two level rules cannot drive the same action") {
  persisted::ControllerConfig config = with_actions({"red_zone"});
  config.rules.push_back(rpm_above("red_zone", 6000.0F));
  config.rules.push_back(rpm_range("red_zone", NumericRange{0.0F, 6500.0F}));

  check_error(config, ValidationError::DuplicateAction, ConfigSection::Rules, 1);
}

TEST_CASE("event rules may share an action with each other and with a level rule") {
  persisted::ControllerConfig config = with_actions({"chime"});
  const persisted::Condition condition{"vehicle.door_open", Comparison::Equal,
                                       persisted::BooleanOperand{true}};
  config.rules.push_back(persisted::StateRule{"chime", condition, FreshnessRequirement::Fresh});
  config.rules.push_back(persisted::EventRule{"chime", condition, EventEdge::BecomesTrue,
                                              FreshnessRequirement::Fresh});
  config.rules.push_back(persisted::EventRule{"chime", condition, EventEdge::BecomesFalse,
                                              FreshnessRequirement::Fresh});

  CHECK(persisted::validate(config).ok());
}

TEST_CASE("a rule needs a signal key") {
  persisted::ControllerConfig config = with_actions({"left_turn", "rpm_fill"});
  persisted::StateRule rule = state_rule("left_turn", persisted::ChoiceOperand{"left"});
  rule.condition.signal_key.clear();
  config.rules.push_back(rule);
  check_error(config, ValidationError::EmptySignalKey, ConfigSection::Rules, 0);

  persisted::RangeRule range = rpm_range("rpm_fill", NumericRange{0.0F, 6500.0F});
  range.signal_key.clear();
  config.rules = {range};
  check_error(config, ValidationError::EmptySignalKey, ConfigSection::Rules, 0);
}

TEST_CASE("brake Boolean rules retain owner opted unverified freshness with LED outputs") {
  const persisted::Condition condition{"vehicle.brake_pressed", Comparison::Equal,
                                       persisted::BooleanOperand{true}};
  const persisted::Rule rules[] = {
      persisted::StateRule{"brake_light", condition, FreshnessRequirement::FreshOrUnverified},
      persisted::EventRule{"brake_light", condition, EventEdge::BecomesTrue,
                           FreshnessRequirement::FreshOrUnverified},
      persisted::SampledStateRule{"brake_light", condition,
                                  FreshnessRequirement::FreshOrUnverified}};
  for (const auto &rule : rules) {
    CAPTURE(persisted::type_of(rule));
    auto config = with_actions({"brake_light"});
    config.rules = {rule};
    config.outputs = {persisted::LedEffectBinding{"brake_light", LedEffect::Brake, 150}};
    CHECK(persisted::validate(config).ok());
    std::visit(
        [](const auto &item) { CHECK(item.freshness == FreshnessRequirement::FreshOrUnverified); },
        config.rules[0]);
    config.outputs.clear();
    CHECK(persisted::validate(config).ok());
    std::visit([](auto &item) { item.freshness = FreshnessRequirement::Fresh; }, config.rules[0]);
    CHECK(persisted::validate(config).ok());
    std::visit([](auto &item) { item.freshness = enum_value<FreshnessRequirement>(99); },
               config.rules[0]);
    check_error(config, ValidationError::UnknownFreshness, ConfigSection::Rules, 0);
  }
}

TEST_CASE("a rule rejects unrecognized comparison, freshness and edge values") {
  persisted::ControllerConfig config = with_actions({"left_turn"});

  persisted::StateRule comparison = state_rule("left_turn", persisted::ChoiceOperand{"left"});
  comparison.condition.comparison = enum_value<Comparison>(99);
  config.rules = {comparison};
  check_error(config, ValidationError::UnknownComparison, ConfigSection::Rules, 0);

  persisted::StateRule freshness = state_rule("left_turn", persisted::ChoiceOperand{"left"});
  freshness.freshness = enum_value<FreshnessRequirement>(99);
  config.rules = {freshness};
  check_error(config, ValidationError::UnknownFreshness, ConfigSection::Rules, 0);

  persisted::EventRule edge{"left_turn", comparison.condition, enum_value<EventEdge>(99),
                            FreshnessRequirement::Fresh};
  edge.condition.comparison = Comparison::Equal;
  config.rules = {edge};
  check_error(config, ValidationError::UnknownEventEdge, ConfigSection::Rules, 0);
}

TEST_CASE("a choice operand needs a key") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  config.rules.push_back(state_rule("left_turn", persisted::ChoiceOperand{""}));

  check_error(config, ValidationError::EmptyChoice, ConfigSection::Rules, 0);
}

TEST_CASE("a number operand must be finite") {
  for (const float value : {kNaN, kInfinity, -kInfinity}) {
    CAPTURE(value);
    persisted::ControllerConfig config = with_actions({"red_zone"});
    config.rules.push_back(rpm_above("red_zone", value));
    check_error(config, ValidationError::InvalidOperand, ConfigSection::Rules, 0);
  }
}

TEST_CASE("ordered comparisons apply only to number operands") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  config.rules.push_back(
      state_rule("left_turn", persisted::ChoiceOperand{"left"}, Comparison::Greater));
  check_error(config, ValidationError::UnsupportedComparison, ConfigSection::Rules, 0);

  config.rules = {
      state_rule("left_turn", persisted::BooleanOperand{true}, Comparison::LessOrEqual)};
  check_error(config, ValidationError::UnsupportedComparison, ConfigSection::Rules, 0);

  config.rules = {state_rule("left_turn", persisted::BooleanOperand{true}, Comparison::NotEqual)};
  CHECK(persisted::validate(config).ok());
}

TEST_CASE("a release threshold must fall back from the activation threshold") {
  persisted::ControllerConfig config = with_actions({"red_zone"});

  config.rules = {rpm_above("red_zone", 6000.0F, 5800.0F)};
  CHECK(persisted::validate(config).ok());

  for (const float release : {6000.0F, 6200.0F, kNaN, kInfinity}) {
    CAPTURE(release);
    config.rules = {rpm_above("red_zone", 6000.0F, release)};
    check_error(config, ValidationError::InvalidHysteresis, ConfigSection::Rules, 0);
  }
}

TEST_CASE("a release threshold above a less-than activation threshold is valid") {
  persisted::ControllerConfig config = with_actions({"low_rpm"});
  persisted::SampledStateRule rule = rpm_above("low_rpm", 800.0F, 900.0F);
  rule.condition.comparison = Comparison::LessOrEqual;
  config.rules = {rule};
  CHECK(persisted::validate(config).ok());

  rule.release_threshold = 700.0F;
  config.rules = {rule};
  check_error(config, ValidationError::InvalidHysteresis, ConfigSection::Rules, 0);
}

TEST_CASE("a release threshold needs an ordered numeric condition") {
  persisted::ControllerConfig config = with_actions({"idle"});
  persisted::SampledStateRule rule = rpm_above("idle", 800.0F, 700.0F);
  rule.condition.comparison = Comparison::Equal;
  config.rules = {rule};

  check_error(config, ValidationError::InvalidHysteresis, ConfigSection::Rules, 0);
}

TEST_CASE("a range needs finite bounds and an ascending input") {
  const NumericRange invalid_inputs[] = {
      {6500.0F, 0.0F},
      {100.0F, 100.0F},
      {kNaN, 1.0F},
      {0.0F, kInfinity},
      {-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}};
  for (const NumericRange &input : invalid_inputs) {
    CAPTURE(input.from);
    CAPTURE(input.to);
    persisted::ControllerConfig config = with_actions({"rpm_fill"});
    config.rules.push_back(rpm_range("rpm_fill", input));
    check_error(config, ValidationError::InvalidRange, ConfigSection::Rules, 0);
  }

  persisted::ControllerConfig config = with_actions({"rpm_fill"});
  config.rules.push_back(rpm_range("rpm_fill", NumericRange{0.0F, 6500.0F}, {0.0F, kNaN}));
  check_error(config, ValidationError::InvalidRange, ConfigSection::Rules, 0);
}

TEST_CASE("a range output may descend or be constant") {
  persisted::ControllerConfig config = with_actions({"rpm_fill"});
  config.rules = {rpm_range("rpm_fill", NumericRange{0.0F, 6500.0F}, NumericRange{1.0F, 0.0F})};
  CHECK(persisted::validate(config).ok());

  config.rules = {rpm_range("rpm_fill", NumericRange{0.0F, 6500.0F}, NumericRange{0.5F, 0.5F})};
  CHECK(persisted::validate(config).ok());
}

// --- Outputs ---------------------------------------------------------------

TEST_CASE("an output must name a declared action") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  config.outputs.push_back(persisted::LedEffectBinding{"left_turn", LedEffect::RightTurn, 100});
  config.outputs.push_back(fill("rpm_fill", persisted::LedZone{0, 100, FillDirection::CenterOut}));

  check_error(config, ValidationError::UndeclaredAction, ConfigSection::Outputs, 1);
}

TEST_CASE("an effect output rejects an unrecognized effect") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  config.outputs.push_back(
      persisted::LedEffectBinding{"left_turn", enum_value<LedEffect>(99), 100});

  check_error(config, ValidationError::UnknownLedEffect, ConfigSection::Outputs, 0);
}

TEST_CASE("priorities are limited to 0..255") {
  persisted::ControllerConfig config = with_actions({"left_turn"});
  for (const persisted::Integer priority : {persisted::Integer{0}, persisted::Integer{255}}) {
    CAPTURE(priority);
    config.outputs = {persisted::LedEffectBinding{"left_turn", LedEffect::RightTurn, priority}};
    CHECK(persisted::validate(config).ok());
  }
  for (const persisted::Integer priority : {persisted::Integer{-1}, persisted::Integer{256}}) {
    CAPTURE(priority);
    config.outputs = {persisted::LedEffectBinding{"left_turn", LedEffect::RightTurn, priority}};
    check_error(config, ValidationError::InvalidPriority, ConfigSection::Outputs, 0);

    persisted::LedFillBinding binding = fill("left_turn", {0, 10, FillDirection::StartToEnd});
    binding.priority = priority;
    config.outputs = {binding};
    check_error(config, ValidationError::InvalidPriority, ConfigSection::Outputs, 0);
  }
}

TEST_CASE("the same action cannot bind the same effect twice") {
  persisted::ControllerConfig config = with_actions({"hazard"});
  config.outputs.push_back(persisted::LedEffectBinding{"hazard", LedEffect::LeftTurn, 100});
  config.outputs.push_back(persisted::LedEffectBinding{"hazard", LedEffect::RightTurn, 100});
  CHECK(persisted::validate(config).ok());

  config.outputs.push_back(persisted::LedEffectBinding{"hazard", LedEffect::LeftTurn, 120});
  check_error(config, ValidationError::DuplicateBinding, ConfigSection::Outputs, 2);
}

TEST_CASE("a fill zone must lie inside the logical strip") {
  const persisted::LedZone valid_zones[] = {{0, 100, FillDirection::CenterOut},
                                            {99, 1, FillDirection::StartToEnd},
                                            {10, 20, FillDirection::EndToStart}};
  for (const persisted::LedZone &zone : valid_zones) {
    persisted::ControllerConfig config = with_actions({"rpm_fill"});
    config.outputs.push_back(fill("rpm_fill", zone));
    CHECK(persisted::validate(config).ok());
  }

  const persisted::LedZone out_of_range[] = {
      {99, 2, FillDirection::StartToEnd},
      {100, 1, FillDirection::StartToEnd},
      {0, 101, FillDirection::StartToEnd},
      {-1, 10, FillDirection::StartToEnd},
      {0, -1, FillDirection::StartToEnd},
      {std::numeric_limits<persisted::Integer>::max(), 1, FillDirection::StartToEnd}};
  for (const persisted::LedZone &zone : out_of_range) {
    CAPTURE(zone.start);
    CAPTURE(zone.length);
    persisted::ControllerConfig config = with_actions({"rpm_fill"});
    config.outputs.push_back(fill("rpm_fill", zone));
    check_error(config, ValidationError::ZoneOutOfRange, ConfigSection::Outputs, 0);
  }
}

TEST_CASE("a fill zone must be non-empty with a recognized direction") {
  persisted::ControllerConfig config = with_actions({"rpm_fill"});
  config.outputs = {fill("rpm_fill", persisted::LedZone{0, 0, FillDirection::StartToEnd})};
  check_error(config, ValidationError::EmptyZone, ConfigSection::Outputs, 0);

  config.outputs = {fill("rpm_fill", persisted::LedZone{0, 10, enum_value<FillDirection>(99)})};
  check_error(config, ValidationError::UnknownFillDirection, ConfigSection::Outputs, 0);
}

TEST_CASE("fill colour channels are limited to 0..255") {
  persisted::ControllerConfig config = with_actions({"rpm_fill"});
  persisted::LedFillBinding binding = fill("rpm_fill", {0, 100, FillDirection::CenterOut});
  binding.color = persisted::Rgb{255, 0, 255};
  config.outputs = {binding};
  CHECK(persisted::validate(config).ok());

  const persisted::Rgb invalid[] = {{256, 0, 0}, {0, -1, 0}, {0, 0, 1000}};
  for (const persisted::Rgb &color : invalid) {
    binding.color = color;
    config.outputs = {binding};
    check_error(config, ValidationError::InvalidColor, ConfigSection::Outputs, 0);
  }
}

TEST_CASE("the same action cannot fill the same zone twice") {
  persisted::ControllerConfig config = with_actions({"rpm_fill"});
  config.outputs.push_back(fill("rpm_fill", {0, 50, FillDirection::StartToEnd}));
  config.outputs.push_back(fill("rpm_fill", {50, 50, FillDirection::EndToStart}));
  config.outputs.push_back(fill("rpm_fill", {0, 50, FillDirection::CenterOut}));
  CHECK(persisted::validate(config).ok());

  config.outputs.push_back(fill("rpm_fill", {0, 50, FillDirection::StartToEnd}));
  check_error(config, ValidationError::DuplicateBinding, ConfigSection::Outputs, 3);
}

TEST_CASE("validation reports the first error in document order") {
  persisted::ControllerConfig config = with_actions({"left_turn", "left_turn"});
  config.version = 2;
  config.rules.push_back(state_rule("missing", persisted::ChoiceOperand{"left"}));
  check_error(config, ValidationError::UnsupportedVersion, ConfigSection::Document, 0);

  config.version = 1;
  check_error(config, ValidationError::DuplicateActionName, ConfigSection::Actions, 1);

  config.actions.pop_back();
  config.outputs.push_back(persisted::LedEffectBinding{"missing", LedEffect::Brake, 100});
  check_error(config, ValidationError::UndeclaredAction, ConfigSection::Rules, 0);
}

} // namespace
