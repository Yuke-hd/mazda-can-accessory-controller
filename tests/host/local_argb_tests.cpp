#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "local_argb/local_argb.h"
#include "mazda/decoder.hpp"
#include "semantic_led_policy.h"

namespace {

class FakeSink final : public local_argb::PixelSink {
public:
  bool write(const local_argb::Rgb color) noexcept override {
    attempts.push_back(color);
    if (failures_remaining > 0) {
      --failures_remaining;
      return false;
    }
    return true;
  }

  std::vector<local_argb::Rgb> attempts;
  int failures_remaining{0};
};

local_argb::SemanticSnapshot valid(const mazda::TurnState turn,
                                   const vehicle_core::MonotonicTimestamp timestamp = 100) {
  return {turn, vehicle_core::SignalStatus::Valid, timestamp, local_argb::SemanticHealth::Online};
}

vehicle_core::RawCanFrame frame(const std::uint32_t id,
                                const vehicle_core::MonotonicTimestamp timestamp,
                                const std::uint8_t dlc = 8) {
  vehicle_core::RawCanFrame result{};
  result.timestamp_us = timestamp;
  result.identifier = id;
  result.dlc = dlc;
  return result;
}

vehicle_core::RawCanFrame turn_frame(const vehicle_core::MonotonicTimestamp timestamp,
                                     const bool left) {
  auto result = frame(mazda::candidate::kTurnSwitchId, timestamp);
  result.data[1] = left ? 1U << 5U : 1U << 4U;
  return result;
}

} // namespace

TEST_CASE("startup transmits an explicit black frame") {
  FakeSink sink;
  local_argb::Controller controller{sink};
  CHECK(controller.start());
  REQUIRE(sink.attempts.size() == 1);
  CHECK(sink.attempts[0] == local_argb::kBlack);
}

TEST_CASE("semantic states map to bounded diagnostic colors") {
  CHECK(local_argb::color_for(valid(mazda::TurnState::Left), 100) == local_argb::kLeftGreen);
  CHECK(local_argb::color_for(valid(mazda::TurnState::Right), 100) == local_argb::kRightBlue);
  CHECK(local_argb::color_for(valid(mazda::TurnState::Hazard), 100) == local_argb::kHazardAmber);
  CHECK(local_argb::color_for(valid(mazda::TurnState::Off), 100) == local_argb::kBlack);

  const local_argb::Rgb colors[]{local_argb::kBlack, local_argb::kLeftGreen, local_argb::kRightBlue,
                                 local_argb::kHazardAmber};
  for (const auto color : colors) {
    CHECK(color.red <= local_argb::kBrightnessCeiling);
    CHECK(color.green <= local_argb::kBrightnessCeiling);
    CHECK(color.blue <= local_argb::kBrightnessCeiling);
  }
}

TEST_CASE("unknown stale offline and decoder error fail black") {
  auto snapshot = valid(mazda::TurnState::Left);
  snapshot.turn_status = vehicle_core::SignalStatus::Unknown;
  CHECK(local_argb::color_for(snapshot, 100) == local_argb::kBlack);
  snapshot.turn_status = vehicle_core::SignalStatus::Stale;
  CHECK(local_argb::color_for(snapshot, 100) == local_argb::kBlack);
  snapshot.turn_status = vehicle_core::SignalStatus::Valid;
  snapshot.health = local_argb::SemanticHealth::CanOffline;
  CHECK(local_argb::color_for(snapshot, 100) == local_argb::kBlack);
  snapshot.health = local_argb::SemanticHealth::DecoderError;
  CHECK(local_argb::color_for(snapshot, 100) == local_argb::kBlack);
}

TEST_CASE("freshness is inclusive at 250000 us and clears at 250001 us") {
  const auto snapshot = valid(mazda::TurnState::Left, 1'000);
  CHECK(local_argb::color_for(snapshot, 251'000) == local_argb::kLeftGreen);
  CHECK(local_argb::color_for(snapshot, 251'001) == local_argb::kBlack);
}

TEST_CASE("controller independently clears stale state and recovers same direction") {
  FakeSink sink;
  local_argb::Controller controller{sink};
  REQUIRE(controller.start());
  REQUIRE(controller.apply(valid(mazda::TurnState::Left, 10), 10));
  REQUIRE(controller.tick(250'011));
  CHECK(sink.attempts.back() == local_argb::kBlack);
  REQUIRE(controller.apply(valid(mazda::TurnState::Left, 300'000), 300'000));
  CHECK(sink.attempts.back() == local_argb::kLeftGreen);
}

TEST_CASE("duplicate semantic submissions avoid redundant hardware refreshes") {
  FakeSink sink;
  local_argb::Controller controller{sink};
  REQUIRE(controller.start());
  const auto snapshot = valid(mazda::TurnState::Right, 50);
  REQUIRE(controller.apply(snapshot, 50));
  const auto writes = sink.attempts.size();
  REQUIRE(controller.apply(snapshot, 60));
  REQUIRE(controller.tick(70));
  CHECK(sink.attempts.size() == writes);
}

TEST_CASE("length-one mailbox overwrites backpressure with newest semantics") {
  local_argb::Mailbox mailbox;
  mailbox.submit(valid(mazda::TurnState::Left, 1));
  mailbox.submit(valid(mazda::TurnState::Hazard, 2));
  local_argb::SemanticSnapshot result{};
  REQUIRE(mailbox.take(result));
  CHECK(result.turn == mazda::TurnState::Hazard);
  CHECK(result.turn_last_update_us == 2);
  CHECK_FALSE(mailbox.take(result));
}

TEST_CASE("driver failure attempts black and retries only after recovery input") {
  FakeSink sink;
  local_argb::Controller controller{sink};
  REQUIRE(controller.start());
  sink.failures_remaining = 1;
  CHECK_FALSE(controller.apply(valid(mazda::TurnState::Left), 100));
  REQUIRE(sink.attempts.size() == 3);
  CHECK(sink.attempts[1] == local_argb::kLeftGreen);
  CHECK(sink.attempts[2] == local_argb::kBlack);
  CHECK(controller.faulted());
  REQUIRE(controller.tick(110));
  CHECK(sink.attempts.size() == 3);
  REQUIRE(controller.apply(valid(mazda::TurnState::Left, 120), 120));
  CHECK(sink.attempts.back() == local_argb::kLeftGreen);
}

TEST_CASE("failed startup clear is retried until black succeeds") {
  FakeSink sink;
  sink.failures_remaining = 1;
  local_argb::Controller controller{sink};
  CHECK_FALSE(controller.start());
  CHECK(controller.faulted());
  CHECK(controller.tick(1));
  REQUIRE(sink.attempts.size() == 2);
  CHECK(std::all_of(sink.attempts.begin(), sink.attempts.end(),
                    [](const local_argb::Rgb color) { return color == local_argb::kBlack; }));
}

TEST_CASE("failed fallback clear blocks color until black succeeds") {
  FakeSink sink;
  local_argb::Controller controller{sink};
  REQUIRE(controller.start());
  sink.failures_remaining = 2;
  CHECK_FALSE(controller.apply(valid(mazda::TurnState::Left), 100));
  CHECK(controller.faulted());
  REQUIRE(controller.apply(valid(mazda::TurnState::Right, 110), 110));
  CHECK(sink.attempts.back() == local_argb::kBlack);
  CHECK(controller.faulted());
  REQUIRE(controller.apply(valid(mazda::TurnState::Right, 120), 120));
  CHECK(sink.attempts.back() == local_argb::kRightBlue);
  CHECK_FALSE(controller.faulted());
}

TEST_CASE("only a valid turn update recovers decoder error after mixed traffic") {
  weact_app::Context context{};
  weact_app::process_received_frame(context, turn_frame(100, true));
  REQUIRE(context.health == local_argb::SemanticHealth::Online);
  REQUIRE(weact_app::semantic_snapshot(context, 100).turn == mazda::TurnState::Left);

  weact_app::process_received_frame(context, frame(mazda::candidate::kTurnSwitchId, 110, 7));
  REQUIRE(context.health == local_argb::SemanticHealth::DecoderError);
  CHECK(local_argb::color_for(weact_app::semantic_snapshot(context, 110), 110) ==
        local_argb::kBlack);

  auto engine = frame(mazda::candidate::kEngineDataId, 120);
  engine.data[0] = 1;
  weact_app::process_received_frame(context, engine);
  CHECK(context.health == local_argb::SemanticHealth::DecoderError);
  auto gear = frame(mazda::candidate::kGearId, 125);
  gear.data[0] = 4;
  gear.data[4] = 4;
  weact_app::process_received_frame(context, gear);
  CHECK(context.health == local_argb::SemanticHealth::DecoderError);
  weact_app::process_received_frame(context, frame(0x123, 130));
  CHECK(context.health == local_argb::SemanticHealth::DecoderError);

  // A valid replay older than the frame that raised the error stays fail-off.
  weact_app::process_received_frame(context, turn_frame(105, true));
  CHECK(context.health == local_argb::SemanticHealth::DecoderError);

  // A newer, valid update of the same direction is sufficient recovery.
  weact_app::process_received_frame(context, turn_frame(140, true));
  CHECK(context.health == local_argb::SemanticHealth::Online);
  CHECK(local_argb::color_for(weact_app::semantic_snapshot(context, 140), 140) ==
        local_argb::kLeftGreen);
}

TEST_CASE("publication policy emits changes and bounded heartbeat only") {
  local_argb::PublicationPolicy policy;
  const auto left = valid(mazda::TurnState::Left, 10);
  CHECK(policy.should_publish(left, 10));
  CHECK_FALSE(policy.should_publish(left, 10 + local_argb::kPublishHeartbeatUs - 1));
  CHECK(policy.should_publish(left, 10 + local_argb::kPublishHeartbeatUs));
  CHECK_FALSE(policy.should_publish(left, 10 + local_argb::kPublishHeartbeatUs + 1));

  auto refreshed_left = left;
  refreshed_left.turn_last_update_us = 20;
  CHECK_FALSE(policy.should_publish(refreshed_left, 10 + local_argb::kPublishHeartbeatUs + 2));
  auto failed = refreshed_left;
  failed.health = local_argb::SemanticHealth::DecoderError;
  CHECK(policy.should_publish(failed, 10 + local_argb::kPublishHeartbeatUs + 3));
}

TEST_CASE("engine gear and unrelated frames do not wake LED publication") {
  weact_app::Context context{};
  local_argb::PublicationPolicy policy;
  weact_app::process_received_frame(context, turn_frame(100, true));
  REQUIRE(policy.should_publish(weact_app::semantic_snapshot(context, 100), 100));

  auto engine = frame(mazda::candidate::kEngineDataId, 110);
  engine.data[0] = 1;
  weact_app::process_received_frame(context, engine);
  CHECK_FALSE(policy.should_publish(weact_app::semantic_snapshot(context, 110), 110));

  auto gear = frame(mazda::candidate::kGearId, 120);
  gear.data[0] = 4;
  gear.data[4] = 4;
  weact_app::process_received_frame(context, gear);
  CHECK_FALSE(policy.should_publish(weact_app::semantic_snapshot(context, 120), 120));
  weact_app::process_received_frame(context, frame(0x123, 130));
  CHECK_FALSE(policy.should_publish(weact_app::semantic_snapshot(context, 130), 130));

  weact_app::process_received_frame(context, turn_frame(140, true));
  CHECK_FALSE(policy.should_publish(weact_app::semantic_snapshot(context, 140), 140));
  CHECK(policy.should_publish(weact_app::semantic_snapshot(context, 100'100), 100'100));
}

TEST_CASE("driver watchdog requests bounded restart and can be disarmed") {
  local_argb::DriverWatchdog watchdog;
  watchdog.begin(50);
  CHECK_FALSE(watchdog.restart_due(50 + local_argb::kDriverHangRestartUs));
  CHECK(watchdog.restart_due(51 + local_argb::kDriverHangRestartUs));
  CHECK(local_argb::kDriverRestartRequestBoundUs == 110'000);
  watchdog.end();
  CHECK_FALSE(watchdog.restart_due(1'000'000));

  watchdog.begin(500);
  CHECK(watchdog.restart_due(499));
}

TEST_CASE("worker progress lease expires and disarms deterministically") {
  local_argb::WorkerLease lease;
  lease.heartbeat(500'000);
  CHECK_FALSE(lease.restart_due(1'000'000));

  lease.arm(50);
  CHECK_FALSE(lease.restart_due(50 + local_argb::kWorkerStallRestartUs));
  CHECK(lease.restart_due(51 + local_argb::kWorkerStallRestartUs));
  CHECK(local_argb::kWorkerRestartRequestBoundUs == 110'000);

  lease.heartbeat(100'000);
  CHECK_FALSE(lease.restart_due(100'000 + local_argb::kWorkerStallRestartUs));
  CHECK(lease.restart_due(100'001 + local_argb::kWorkerStallRestartUs));
  lease.disarm();
  lease.heartbeat(900'000);
  CHECK_FALSE(lease.restart_due(1'000'000));

  lease.arm(500);
  CHECK(lease.restart_due(499));
}
