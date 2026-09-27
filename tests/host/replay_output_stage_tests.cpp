#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "action_engine/engine.hpp"
#include "replay/controller.hpp"
#include "replay/output_stage.hpp"
#include "replay/scheduler.hpp"

// The replay core drives any OutputStage: this fake names no local ARGB type
// and records the lifecycle and action sequence the controller produces.

namespace {

constexpr std::uint32_t kTurnSwitchId = 0x091;
constexpr action_engine::ActionId kLeftTurnAction{7};

enum class Call : std::uint8_t { Configure, Start, Action, Tick, FailOff, Stop };

struct Record {
  Call call{Call::Configure};
  vehicle_core::MonotonicTimestamp time_us{0};
  action_engine::ActionCommand command{};
};

class FakeOutputStage final : public replay::OutputStage, public action_engine::ActionSink {
public:
  explicit FakeOutputStage(const replay::ReplayClock &clock) noexcept : clock_(&clock) {}

  bool configure(action_engine::ActionEngine &engine) noexcept override {
    records_.push_back({Call::Configure, clock_->now(), {}});
    if (!configure_ok_)
      return false;
    return engine.add_sink(*this) == action_engine::ConfigStatus::Ok &&
           engine.add_state_rule({{"vehicle.turn_state", action_engine::Comparison::Equal,
                                   action_engine::RuleOperand::choice("left")},
                                  kLeftTurnAction}) == action_engine::ConfigStatus::Ok;
  }
  bool start(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    records_.push_back({Call::Start, now_us, {}});
    return start_ok_;
  }
  bool tick(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    records_.push_back({Call::Tick, now_us, {}});
    return tick_ok_;
  }
  bool fail_off(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    records_.push_back({Call::FailOff, now_us, {}});
    return true;
  }
  bool stop(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    records_.push_back({Call::Stop, now_us, {}});
    return true;
  }
  vehicle_core::Microseconds tick_period_us() const noexcept override { return 25'000; }

  void execute(const action_engine::ActionCommand &command) noexcept override {
    records_.push_back({Call::Action, clock_->now(), command});
  }

  void reject_configuration() noexcept { configure_ok_ = false; }
  void fail_start() noexcept { start_ok_ = false; }
  void fail_ticks() noexcept { tick_ok_ = false; }

  [[nodiscard]] std::vector<Call> calls() const {
    std::vector<Call> calls;
    std::transform(records_.begin(), records_.end(), std::back_inserter(calls),
                   [](const Record &record) { return record.call; });
    return calls;
  }
  [[nodiscard]] const std::vector<Record> &records() const noexcept { return records_; }

private:
  const replay::ReplayClock *clock_;
  std::vector<Record> records_{};
  bool configure_ok_{true};
  bool start_ok_{true};
  bool tick_ok_{true};
};

gvret::TimedCanFrame left_turn(const vehicle_core::MonotonicTimestamp timestamp_us) {
  gvret::TimedCanFrame timed{};
  timed.relative_time_us = timestamp_us;
  timed.frame.identifier = kTurnSwitchId;
  timed.frame.timestamp_us = timestamp_us;
  timed.frame.dlc = 8;
  timed.frame.data[1] = 0x20;
  return timed;
}

} // namespace

TEST_CASE("controller drives an injected stage through start, action, tick and stop") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  {
    replay::ReplayController controller{{left_turn(0)}, clock, stage};
    REQUIRE(stage.calls() == std::vector<Call>{Call::Configure});

    REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
    // Attaching the engine publishes the initial (inactive) rule state.
    CHECK(stage.calls() ==
          std::vector<Call>{Call::Configure, Call::Start, Call::Action, Call::Tick});

    REQUIRE(controller.process_next_frame().status == replay::ReplayControllerStatus::Ok);
    REQUIRE(clock.advance_to(10'000));
    REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
    CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  }

  const std::vector<Call> expected{Call::Configure, Call::Start, Call::Action,  Call::Tick,
                                   Call::Action,    Call::Tick,  Call::FailOff, Call::Stop};
  REQUIRE(stage.calls() == expected);
  const auto &records = stage.records();
  CHECK(records[2].command.action == kLeftTurnAction);
  CHECK(records[2].command.kind == action_engine::ActionCommandKind::Deactivate);
  CHECK(records[4].command.action == kLeftTurnAction);
  CHECK(records[4].command.kind == action_engine::ActionCommandKind::Activate);
  CHECK(records[4].time_us == 0);
  CHECK(records[5].time_us == 10'000);
  CHECK(records[6].time_us == 10'000);
  CHECK(records[7].time_us == 10'000);
}

TEST_CASE("a stage whose start fails is failed off and never ticked") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  stage.fail_start();
  {
    replay::ReplayController controller{{left_turn(0)}, clock, stage};
    CHECK(controller.start() == replay::ReplayControllerStatus::OutputFault);
    CHECK_FALSE(controller.running());
  }
  CHECK(stage.calls() == std::vector<Call>{Call::Configure, Call::Start, Call::FailOff});
}

TEST_CASE("a failed tick fails the stage off immediately") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  replay::ReplayController controller{{left_turn(0)}, clock, stage};
  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);

  stage.fail_ticks();
  REQUIRE(clock.advance_to(10'000));
  CHECK(controller.tick_output() == replay::ReplayControllerStatus::OutputFault);
  CHECK_FALSE(controller.running());
  CHECK(stage.calls() == std::vector<Call>{Call::Configure, Call::Start, Call::Action, Call::Tick,
                                           Call::Tick, Call::FailOff});
  CHECK(stage.records().back().time_us == 10'000);

  (void)controller.stop();
  CHECK(stage.calls().back() == Call::Stop);
}

TEST_CASE("a replay failure fails the stage off before stopping it") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  replay::ReplayController controller{{left_turn(0)}, clock, stage};
  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);

  CHECK(controller.process_source_fault().input == replay::ReplayInputResult::Fault);
  CHECK_FALSE(controller.running());
  (void)controller.stop();

  const auto calls = stage.calls();
  const auto fail_off = std::find(calls.begin(), calls.end(), Call::FailOff);
  REQUIRE(fail_off != calls.end());
  CHECK(std::find(calls.begin(), fail_off, Call::Stop) == fail_off);
  CHECK(calls.back() == Call::Stop);
}

TEST_CASE("a stage that rejects configuration is failed off and never attached") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  stage.reject_configuration();
  replay::ReplayController controller{{left_turn(0)}, clock, stage};

  CHECK(controller.start() == replay::ReplayControllerStatus::ConfigurationFailed);
  CHECK(stage.calls() == std::vector<Call>{Call::Configure, Call::Start, Call::FailOff});
}

TEST_CASE("the stage's tick period is the scheduler's default output cadence") {
  replay::ReplayClock clock;
  FakeOutputStage stage{clock};
  const auto result = replay::run_replay({left_turn(0)}, clock, stage, {50'000});

  REQUIRE(result.ok());
  CHECK(result.output_ticks == 3); // 0, 25 ms and 50 ms.
  const auto calls = stage.calls();
  CHECK(std::count(calls.begin(), calls.end(), Call::Action) >= 1);
  REQUIRE(calls.size() >= 2);
  CHECK(calls[calls.size() - 2] == Call::FailOff);
  CHECK(calls.back() == Call::Stop);
}
