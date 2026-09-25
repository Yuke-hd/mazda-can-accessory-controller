#include "action_engine_fixture.hpp"

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "action_engine/engine.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"

// Acceptance: fake provider -> engine -> recording sink on host.

namespace {

using namespace action_engine_fixture;
using action_engine::ActionCommand;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::EventEdge;
using action_engine::EventRuleConfig;
using action_engine::FreshnessRequirement;
using action_engine::RuleOperand;
using action_engine::SignalCondition;
using action_engine::StateRuleConfig;
using test_support::FakeSignalProvider;
using test_support::RecordingActionSink;
using vehicle_signals::SignalStatus;
using Commands = std::vector<ActionCommand>;

static_assert(!std::is_copy_constructible_v<ActionEngine>);
static_assert(!std::is_move_constructible_v<ActionEngine>);
static_assert(ActionEngine::kMaxSinks == 4);
static_assert(ActionEngine::kMaxRules == 16);

constexpr std::uint16_t kCourtesyLight = 1;
constexpr std::uint16_t kReverseChime = 2;
constexpr std::uint16_t kSpeedWarning = 3;
constexpr std::uint16_t kReverseLamp = 4;
constexpr std::uint16_t kDoorClosedChime = 5;

StateRuleConfig door_open_light(FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return StateRuleConfig{
      SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
      ActionId{kCourtesyLight}, freshness};
}
EventRuleConfig reverse_chime() {
  return EventRuleConfig{
      SignalCondition{"transmission.gear", Comparison::Equal, RuleOperand::choice("reverse")},
      EventEdge::BecomesTrue, ActionId{kReverseChime}};
}
StateRuleConfig reverse_lamp() {
  return StateRuleConfig{
      SignalCondition{"transmission.gear", Comparison::Equal, RuleOperand::choice("reverse")},
      ActionId{kReverseLamp}};
}
StateRuleConfig speed_warning() {
  return StateRuleConfig{
      SignalCondition{"motion.speed_kph", Comparison::Greater, RuleOperand::number(30.0F)},
      ActionId{kSpeedWarning}};
}

SignalNotification initial(SignalId id) { return Notice{id}.no_data().initial(); }

// A provider, an engine over it and one recording sink. Setup happens while
// the provider is stopped; publish() needs it running.
struct Bench final {
  Bench() { REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok); }
  ~Bench() {
    provider.stop();
    if (engine.attached()) {
      (void)engine.detach();
    }
  }
  Bench(const Bench &) = delete;
  Bench &operator=(const Bench &) = delete;

  void start() {
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  Commands publish(const SignalNotification &notice) {
    (void)provider.publish(notice);
    return sink.take();
  }

  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  RecordingActionSink sink{};
};

} // namespace

TEST_CASE("fake provider drives state and event rules through the engine into a sink") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_event_rule(reverse_chime()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(reverse_lamp()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(speed_warning()) == ConfigStatus::Ok);
  bench.start();

  // initial: every state rule establishes an explicit baseline; the event
  // rule only records its baseline.
  CHECK(bench.publish(initial(kDoorOpen)) == Commands{deactivate(kCourtesyLight)});
  CHECK(bench.publish(Notice{kGear}.value(SignalValue::enumeration(kDrive)).initial()) ==
        Commands{deactivate(kReverseLamp)});
  CHECK(bench.publish(initial(kSpeed)) == Commands{deactivate(kSpeedWarning)});

  // Boolean, enum choice and number comparisons.
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});
  CHECK(bench.publish(gear(kReverse)) == Commands{trigger(kReverseChime), activate(kReverseLamp)});
  CHECK(bench.publish(speed(30.0F)) == Commands{});
  CHECK(bench.publish(speed(42.5F)) == Commands{activate(kSpeedWarning)});

  // Dedup: unchanged latest states emit nothing.
  CHECK(bench.publish(door(true)) == Commands{});
  CHECK(bench.publish(gear(kReverse)) == Commands{});
  CHECK(bench.publish(speed(50.0F)) == Commands{});

  // Fault: Stale, Unavailable and NoData fail off; the event rule never fires.
  CHECK(bench.publish(door(true, Availability::Stale)) == Commands{deactivate(kCourtesyLight)});
  CHECK(bench.publish(Notice{kGear}
                          .value(SignalValue::enumeration(kReverse), Availability::Unavailable)
                          .became_unavailable()) == Commands{deactivate(kReverseLamp)});
  CHECK(bench.publish(Notice{kSpeed}.no_data()) == Commands{deactivate(kSpeedWarning)});

  // Recovery re-evaluates the latest state; a recovered notice never triggers.
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});
  CHECK(bench.publish(Notice{kGear}.value(SignalValue::enumeration(kReverse)).recovered()) ==
        Commands{activate(kReverseLamp)});
  CHECK(bench.publish(speed(31.0F)) == Commands{activate(kSpeedWarning)});

  // Coalesced drive -> reverse -> drive -> reverse: the latest state equals
  // the last output, so nothing is replayed.
  CHECK(bench.publish(Notice{kGear}.value(SignalValue::enumeration(kReverse)).coalesced()) ==
        Commands{});
  // Coalesced reverse -> drive: one explicit transition.
  CHECK(bench.publish(Notice{kGear}.value(SignalValue::enumeration(kDrive)).coalesced()) ==
        Commands{deactivate(kReverseLamp)});
  CHECK(bench.publish(gear(kReverse)) == Commands{trigger(kReverseChime), activate(kReverseLamp)});
}

TEST_CASE("FreshnessUnverified door readings follow each rule's explicit policy") {
  Bench bench{};
  constexpr std::uint16_t kLenientLight = 8;
  StateRuleConfig lenient = door_open_light(FreshnessRequirement::FreshOrUnverified);
  lenient.action = ActionId{kLenientLight};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(lenient) == ConfigStatus::Ok);
  bench.start();

  CHECK(bench.publish(Notice{kDoorOpen}
                          .value(SignalValue::boolean(true), Availability::FreshnessUnverified)
                          .initial()) ==
        Commands{deactivate(kCourtesyLight), activate(kLenientLight)});
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});
  CHECK(bench.publish(door(true, Availability::FreshnessUnverified)) ==
        Commands{deactivate(kCourtesyLight)});
}

TEST_CASE("every registered sink receives every command in registration order") {
  Bench bench{};
  RecordingActionSink second{};
  REQUIRE(bench.engine.add_sink(second) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  bench.start();

  (void)bench.provider.publish(door(true));
  CHECK(bench.sink.commands() == Commands{activate(kCourtesyLight)});
  CHECK(second.commands() == Commands{activate(kCourtesyLight)});
}

TEST_CASE("three rules on transmission.gear share one provider subscription") {
  Bench bench{};
  EventRuleConfig out_of_reverse = reverse_chime();
  out_of_reverse.edge = EventEdge::BecomesFalse;
  out_of_reverse.action = ActionId{kDoorClosedChime};
  REQUIRE(bench.engine.add_event_rule(reverse_chime()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(reverse_lamp()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_event_rule(out_of_reverse) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  bench.start();

  CHECK(bench.provider.subscriber_count(kGear) == 1);
  CHECK(bench.provider.subscriber_count(kDoorOpen) == 1);
  CHECK(bench.provider.subscription_count() == 2);
  CHECK(bench.publish(gear(kDrive)) == Commands{deactivate(kReverseLamp)});
  CHECK(bench.publish(gear(kReverse)) == Commands{trigger(kReverseChime), activate(kReverseLamp)});
  CHECK(bench.publish(gear(kPark)) ==
        Commands{deactivate(kReverseLamp), trigger(kDoorClosedChime)});
}

TEST_CASE("notices for a signal without rules produce no commands") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  bench.start();
  CHECK(bench.provider.subscriber_count(kSpeed) == 0);
  CHECK(bench.publish(speed(90.0F)) == Commands{});
}

TEST_CASE("configuration errors surface from add_state_rule and add_event_rule") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};

  StateRuleConfig no_action = door_open_light();
  no_action.action = ActionId{};
  CHECK(engine.add_state_rule(no_action) == ConfigStatus::InvalidAction);

  StateRuleConfig unknown = door_open_light();
  unknown.condition.signal_key = "body.sunroof_open";
  CHECK(engine.add_state_rule(unknown) == ConfigStatus::UnknownSignal);

  EventRuleConfig sport = reverse_chime();
  sport.condition.operand = RuleOperand::choice("sport");
  CHECK(engine.add_event_rule(sport) == ConfigStatus::UnknownChoice);

  EventRuleConfig odometer = reverse_chime();
  odometer.condition =
      SignalCondition{"trip.odometer_km", Comparison::Greater, RuleOperand::number(1000.0F)};
  CHECK(engine.add_event_rule(odometer) == ConfigStatus::UnsupportedCapability);

  EventRuleConfig no_event_action = reverse_chime();
  no_event_action.action = ActionId{};
  CHECK(engine.add_event_rule(no_event_action) == ConfigStatus::InvalidAction);

  // Rejected rules take no capacity and create no subscription.
  CHECK(engine.attach() == SignalStatus::Ok);
  CHECK(provider.subscription_count() == 0);
  CHECK(engine.detach() == SignalStatus::Ok);
}

TEST_CASE("the seventeenth rule and the fifth sink exceed capacity") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  for (std::size_t index = 0; index < ActionEngine::kMaxRules; ++index) {
    REQUIRE(engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  }
  CHECK(engine.add_state_rule(door_open_light()) == ConfigStatus::CapacityExceeded);
  CHECK(engine.add_event_rule(reverse_chime()) == ConfigStatus::CapacityExceeded);

  std::array<RecordingActionSink, ActionEngine::kMaxSinks + 1> sinks{};
  for (std::size_t index = 0; index < ActionEngine::kMaxSinks; ++index) {
    REQUIRE(engine.add_sink(sinks[index]) == ConfigStatus::Ok);
  }
  CHECK(engine.add_sink(sinks.back()) == ConfigStatus::CapacityExceeded);

  // Sixteen rules on one signal still use one subscription.
  REQUIRE(engine.attach() == SignalStatus::Ok);
  CHECK(provider.subscription_count() == 1);
  provider.start();
  (void)provider.publish(door(true));
  for (std::size_t index = 0; index < ActionEngine::kMaxSinks; ++index) {
    CHECK(sinks[index].commands().size() == ActionEngine::kMaxRules);
  }
  CHECK(sinks.back().commands().empty());
  provider.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}

TEST_CASE("sinks and rules are configured only while the engine is detached") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  RecordingActionSink late{};
  CHECK(bench.engine.add_sink(late) == ConfigStatus::InvalidState);
  CHECK(bench.engine.add_state_rule(speed_warning()) == ConfigStatus::InvalidState);
  CHECK(bench.engine.add_event_rule(reverse_chime()) == ConfigStatus::InvalidState);
  CHECK(bench.engine.attach() == SignalStatus::InvalidState);

  REQUIRE(bench.engine.detach() == SignalStatus::Ok);
  CHECK_FALSE(bench.engine.attached());
  CHECK(bench.engine.detach() == SignalStatus::InvalidState);
  CHECK(bench.engine.add_state_rule(speed_warning()) == ConfigStatus::Ok);
}

TEST_CASE("attach while the provider runs is rejected and leaves nothing subscribed") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  bench.provider.start();
  CHECK(bench.engine.attach() == SignalStatus::InvalidState);
  CHECK_FALSE(bench.engine.attached());
  bench.provider.stop();
  CHECK(bench.provider.subscription_count() == 0);
}

TEST_CASE("a full motion.speed_kph channel rolls back the door subscription") {
  Bench bench{};
  RecordingActionSink other{};
  std::array<vehicle_signals::SignalSubscription, 2> occupied{};
  for (auto &token : occupied) {
    const auto result = bench.provider.subscribe(
        kSpeed, [](void *, const SignalNotification &) noexcept {}, &other);
    REQUIRE(result.ok());
    token = *result.value;
  }
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(speed_warning()) == ConfigStatus::Ok);

  CHECK(bench.engine.attach() == SignalStatus::CapacityExceeded);
  CHECK_FALSE(bench.engine.attached());
  CHECK(bench.provider.subscriber_count(kDoorOpen) == 0);
  CHECK(bench.provider.subscription_count() == 2);

  // Once a slot is free, the same engine attaches.
  REQUIRE(bench.provider.unsubscribe(occupied[0]).ok());
  CHECK(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.provider.subscriber_count(kDoorOpen) == 1);
  CHECK(bench.provider.subscriber_count(kSpeed) == 2);
}

TEST_CASE("a failed rollback keeps the engine attached so detach can retry") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_state_rule(speed_warning()) == ConfigStatus::Ok);
  for (int index = 0; index < 2; ++index) {
    REQUIRE(bench.provider
                .subscribe(
                    kSpeed, [](void *, const SignalNotification &) noexcept {}, nullptr)
                .ok());
  }
  bench.provider.fail_next_unsubscribe(SignalStatus::Timeout);

  CHECK(bench.engine.attach() == SignalStatus::CapacityExceeded);
  CHECK(bench.engine.attached());
  CHECK(bench.provider.subscriber_count(kDoorOpen) == 1);
  CHECK(bench.engine.detach() == SignalStatus::Ok);
  CHECK(bench.provider.subscriber_count(kDoorOpen) == 0);
}

TEST_CASE("detach while the provider runs fails, keeps subscriptions and is retryable") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_event_rule(reverse_chime()) == ConfigStatus::Ok);
  bench.start();

  CHECK(bench.engine.detach() == SignalStatus::InvalidState);
  CHECK(bench.engine.attached());
  CHECK(bench.provider.subscription_count() == 2);
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});

  bench.provider.stop();
  bench.provider.fail_next_unsubscribe(SignalStatus::Timeout);
  CHECK(bench.engine.detach() == SignalStatus::Timeout);
  CHECK(bench.engine.attached());
  CHECK(bench.provider.subscription_count() == 2);

  CHECK(bench.engine.detach() == SignalStatus::Ok);
  CHECK_FALSE(bench.engine.attached());
  CHECK(bench.provider.subscription_count() == 0);
  bench.provider.start();
  CHECK(bench.publish(door(false)) == Commands{});
}

TEST_CASE("re-attaching resets rule state so the next door notice is emitted again") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(door_open_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_event_rule(reverse_chime()) == ConfigStatus::Ok);
  bench.start();
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});
  CHECK(bench.publish(gear(kDrive)) == Commands{});

  bench.provider.stop();
  REQUIRE(bench.engine.detach() == SignalStatus::Ok);
  bench.start();
  CHECK(bench.publish(door(true)) == Commands{activate(kCourtesyLight)});
  // The event baseline was cleared too: reverse right after re-attach is not
  // a transition.
  CHECK(bench.publish(gear(kReverse)) == Commands{});
}
