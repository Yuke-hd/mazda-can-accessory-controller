#include "action_engine_fixture.hpp"

#include <optional>

#include "action_engine/condition.hpp"
#include "action_engine/rules.hpp"

namespace {

using namespace action_engine_fixture;
using action_engine::ActionCommand;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::EventEdge;
using action_engine::EventRule;
using action_engine::FreshnessRequirement;
using action_engine::ResolvedCondition;
using action_engine::StateRule;

constexpr std::uint16_t kCourtesyLight = 7;
constexpr std::uint16_t kChime = 9;

const ResolvedCondition kDoorIsOpen{kDoorOpen, Comparison::Equal, SignalValue::boolean(true),
                                    FreshnessRequirement::Fresh};
const ResolvedCondition kInReverse{kGear, Comparison::Equal, SignalValue::enumeration(kReverse),
                                   FreshnessRequirement::Fresh};

StateRule courtesy_light() { return StateRule{kDoorIsOpen, ActionId{kCourtesyLight}}; }
EventRule reverse_chime(EventEdge edge = EventEdge::BecomesTrue) {
  return EventRule{kInReverse, edge, ActionId{kChime}};
}

std::optional<ActionCommand> none() { return std::nullopt; }

} // namespace

// ---------------------------------------------------------------------------
// State rules: output = actionable && condition holds.

TEST_CASE("state rule reports the signal it watches") {
  CHECK(courtesy_light().signal() == kDoorOpen);
  CHECK(reverse_chime().signal() == kGear);
}

TEST_CASE("an initial open-door notice activates the courtesy light") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(Notice{kDoorOpen}.value(SignalValue::boolean(true)).initial()) ==
        activate(kCourtesyLight));
}

TEST_CASE("an initial NoData door notice explicitly deactivates the courtesy light") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(Notice{kDoorOpen}.no_data().initial()) == deactivate(kCourtesyLight));
}

TEST_CASE("a repeated open-door reading is deduplicated until the door closes") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(true)) == activate(kCourtesyLight));
  CHECK(rule.on_notice(door(true)) == none());
  CHECK(rule.on_notice(door(false)) == deactivate(kCourtesyLight));
  CHECK(rule.on_notice(door(false)) == none());
}

TEST_CASE("the first notice after reset emits even when it matches the last output") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(false)) == deactivate(kCourtesyLight));
  rule.reset();
  CHECK(rule.on_notice(door(false)) == deactivate(kCourtesyLight));
}

TEST_CASE("every initial notice re-establishes the sink baseline even when unchanged") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(true)) == activate(kCourtesyLight));
  CHECK(rule.on_notice(Notice{kDoorOpen}.value(SignalValue::boolean(true)).initial()) ==
        activate(kCourtesyLight));
}

TEST_CASE("a door that becomes Unavailable fails off and recovers on a fresh reading") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(true)) == activate(kCourtesyLight));
  CHECK(rule.on_notice(Notice{kDoorOpen}
                           .value(SignalValue::boolean(true), Availability::Unavailable)
                           .became_unavailable()) == deactivate(kCourtesyLight));
  CHECK(rule.on_notice(Notice{kDoorOpen}.value(SignalValue::boolean(true)).recovered()) ==
        activate(kCourtesyLight));
}

TEST_CASE("a Stale open-door reading fails off without a became_unavailable flag") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(true)) == activate(kCourtesyLight));
  CHECK(rule.on_notice(door(true, Availability::Stale)) == deactivate(kCourtesyLight));
  CHECK(rule.on_notice(door(true, Availability::Stale)) == none());
}

TEST_CASE("an unverified open door fails off under Fresh and activates under FreshOrUnverified") {
  StateRule strict = courtesy_light();
  StateRule lenient{ResolvedCondition{kDoorOpen, Comparison::Equal, SignalValue::boolean(true),
                                      FreshnessRequirement::FreshOrUnverified},
                    ActionId{kCourtesyLight}};
  const auto unverified_open = door(true, Availability::FreshnessUnverified);
  CHECK(strict.on_notice(unverified_open) == deactivate(kCourtesyLight));
  CHECK(lenient.on_notice(unverified_open) == activate(kCourtesyLight));
}

TEST_CASE("a coalesced notice evaluates only the latest state") {
  StateRule rule = courtesy_light();
  CHECK(rule.on_notice(door(true)) == activate(kCourtesyLight));
  // Open -> closed -> open was merged: the latest state is unchanged.
  CHECK(rule.on_notice(Notice{kDoorOpen}.value(SignalValue::boolean(true)).coalesced()) == none());
  CHECK(rule.on_notice(Notice{kDoorOpen}.value(SignalValue::boolean(false)).coalesced()) ==
        deactivate(kCourtesyLight));
}

// ---------------------------------------------------------------------------
// Event rules: a Trigger only for a transition between two consecutive
// actionable readings in the chosen direction.

TEST_CASE("shifting from drive into reverse triggers the reverse chime once") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
  CHECK(rule.on_notice(gear(kReverse)) == none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("a BecomesFalse rule triggers when shifting out of reverse") {
  EventRule rule = reverse_chime(EventEdge::BecomesFalse);
  CHECK(rule.on_notice(gear(kReverse)) == none());
  CHECK(rule.on_notice(gear(kDrive)) == trigger(kChime));
  CHECK(rule.on_notice(gear(kReverse)) == none());
}

TEST_CASE("an initial reverse notice only sets the baseline") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(Notice{kGear}.value(SignalValue::enumeration(kReverse)).initial()) ==
        none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("an initial notice after a drive baseline does not trigger on reverse") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(Notice{kGear}.value(SignalValue::enumeration(kReverse)).initial()) ==
        none());
}

TEST_CASE("the first reading after reset has no baseline and does not trigger") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  rule.reset();
  CHECK(rule.on_notice(gear(kReverse)) == none());
}

TEST_CASE("a Stale gear reading clears the baseline so no transition spans it") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse, Availability::Stale)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("NoData and Unavailable gear notices never trigger and clear the baseline") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(Notice{kGear}.no_data()) == none());
  CHECK(rule.on_notice(gear(kReverse)) == none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse, Availability::Unavailable)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == none());
}

TEST_CASE("a recovered reverse notice re-baselines without triggering") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(Notice{kGear}.value(SignalValue::enumeration(kReverse)).recovered()) ==
        none());
  CHECK(rule.on_notice(gear(kReverse)) == none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("a became_unavailable notice that is usable again is a gap without a trigger") {
  // Usable -> unusable -> usable merged into one notice carries both flags.
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(Notice{kGear}
                           .value(SignalValue::enumeration(kReverse))
                           .became_unavailable()
                           .recovered()
                           .coalesced()) == none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("a became_unavailable flag alone is a gap even when the latest reading is usable") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(
            Notice{kGear}.value(SignalValue::enumeration(kReverse)).became_unavailable()) ==
        none());
  CHECK(rule.on_notice(gear(kDrive)) == none());
  CHECK(rule.on_notice(gear(kReverse)) == trigger(kChime));
}

TEST_CASE("an unverified reverse reading triggers only under FreshOrUnverified") {
  EventRule strict = reverse_chime();
  EventRule lenient{ResolvedCondition{kGear, Comparison::Equal, SignalValue::enumeration(kReverse),
                                      FreshnessRequirement::FreshOrUnverified},
                    EventEdge::BecomesTrue, ActionId{kChime}};
  for (EventRule *rule : {&strict, &lenient}) {
    CHECK(rule->on_notice(gear(kDrive)) == none());
  }
  CHECK(strict.on_notice(gear(kReverse, Availability::FreshnessUnverified)) == none());
  CHECK(lenient.on_notice(gear(kReverse, Availability::FreshnessUnverified)) == trigger(kChime));
}

TEST_CASE("a coalesced notice triggers at most once and never reconstructs merged edges") {
  EventRule rule = reverse_chime();
  CHECK(rule.on_notice(gear(kDrive)) == none());
  // Drive -> reverse merged: latest differs from the baseline, one Trigger.
  CHECK(rule.on_notice(Notice{kGear}.value(SignalValue::enumeration(kReverse)).coalesced()) ==
        trigger(kChime));
  // Reverse -> drive -> reverse merged: the latest equals the baseline, so the
  // merged edges are not reconstructed.
  CHECK(rule.on_notice(Notice{kGear}.value(SignalValue::enumeration(kReverse)).coalesced()) ==
        none());
}
