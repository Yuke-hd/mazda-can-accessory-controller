#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "local_argb/local_argb.h"
#include "replay/controller.hpp"
#include "replay/local_argb_stage.hpp"
#include "vehicle_profile_metadata.hpp"
#include "vehicle_profile_runtime.hpp"
#include "vehicle_profile_runtime/replay_adapter.hpp"

namespace {

using Pixels = std::vector<local_argb::PixelFrame>;

class RecordingPixelSink final : public local_argb::PixelFrameSink {
public:
  bool write(const local_argb::PixelFrame &frame, std::size_t) noexcept override {
    frames_.push_back(frame);
    return true;
  }

  [[nodiscard]] const Pixels &frames() const noexcept { return frames_; }

private:
  Pixels frames_{};
};

class RecordingObserver final : public replay::SignalObserver {
public:
  void on_catalog(const vehicle_signals::SignalCatalogView catalog) noexcept override {
    ++catalog_count;
    catalog_size = catalog.size();
    order.emplace_back("catalog");
  }

  void on_reading(const vehicle_core::MonotonicTimestamp,
                  const vehicle_signals::SignalMetadata &signal,
                  const vehicle_signals::SignalReading &reading) noexcept override {
    order.emplace_back(signal.key);
    if (signal.key == "vehicle.brake_pressed" && reading.value.has_value())
      brake_value = reading.value->as_boolean();
    if (signal.key == "vehicle.engine_rpm" && reading.value.has_value())
      rpm_value = reading.value->as_number();
  }

  std::vector<std::string> order{};
  std::optional<bool> brake_value{};
  std::optional<float> rpm_value{};
  std::size_t catalog_count{0};
  std::size_t catalog_size{0};
};

struct RecordedNotifications final {
  std::array<vehicle_signals::SignalNotification, 8> entries{};
  std::size_t count{0};
};

void record_notification(void *const context,
                         const vehicle_signals::SignalNotification &notification) noexcept {
  auto &recorded = *static_cast<RecordedNotifications *>(context);
  if (recorded.count < recorded.entries.size())
    recorded.entries[recorded.count++] = notification;
}

struct CallbackMutationProbe final {
  vehicle_profile_runtime::ReplayAdapter *adapter{nullptr};
  std::size_t fresh_notices{0};
  vehicle_profile_runtime::ReplayAdapterStatus process_status{
      vehicle_profile_runtime::ReplayAdapterStatus::Ok};
  vehicle_profile_runtime::ReplayAdapterStatus service_status{
      vehicle_profile_runtime::ReplayAdapterStatus::Ok};
  vehicle_profile_runtime::ReplayAdapterStatus fault_status{
      vehicle_profile_runtime::ReplayAdapterStatus::Ok};
  vehicle_profile_runtime::ReplayAdapterStatus stop_status{
      vehicle_profile_runtime::ReplayAdapterStatus::Ok};
  vehicle_signals::Availability availability_after_attempt{vehicle_signals::Availability::NoData};
};

void reject_callback_mutations(void *const context,
                               const vehicle_signals::SignalNotification &notification) noexcept {
  if (notification.current.availability != vehicle_signals::Availability::Fresh)
    return;

  auto &probe = *static_cast<CallbackMutationProbe *>(context);
  ++probe.fresh_notices;
  probe.process_status = probe.adapter->process(vehicle_core::RawCanFrame{});
  probe.service_status = probe.adapter->service();
  probe.fault_status = probe.adapter->source_fault();
  probe.stop_status = probe.adapter->stop();
  const auto reading = probe.adapter->provider().read(notification.id);
  if (reading.ok())
    probe.availability_after_attempt = reading.value->availability;
}

const vehicle_profile_runtime::DispatchEntry *dispatch_for(const vehicle_signals::SignalId id) {
  for (const auto &entry : vehicle_profile::kDispatchTable) {
    for (std::size_t index = 0; index < entry.output_count; ++index) {
      if (entry.output_ids[index] == id)
        return &entry;
    }
  }
  return nullptr;
}

gvret::TimedCanFrame engine_frame(const vehicle_core::MonotonicTimestamp timestamp_us,
                                  const std::uint32_t identifier) {
  gvret::TimedCanFrame timed{};
  timed.relative_time_us = timestamp_us;
  timed.frame.identifier = identifier;
  timed.frame.timestamp_us = timestamp_us;
  timed.frame.dlc = 8;
  // Little-endian raw 13,000 at scale 0.25 is 3,250 rpm.
  timed.frame.data[0] = 0xC8;
  timed.frame.data[1] = 0x32;
  return timed;
}

gvret::TimedCanFrame brake_frame(const vehicle_core::MonotonicTimestamp timestamp_us,
                                 const std::uint32_t identifier, const bool pressed) {
  gvret::TimedCanFrame timed{};
  timed.relative_time_us = timestamp_us;
  timed.frame.identifier = identifier;
  timed.frame.timestamp_us = timestamp_us;
  timed.frame.dlc = 8;
  timed.frame.data[0] = pressed ? 1U : 0U;
  return timed;
}

[[maybe_unused]] vehicle_profile_runtime::ProfileTables selected_tables() {
  return {vehicle_profile::kCatalog,
          vehicle_profile::kFreshnessPolicies.data(),
          vehicle_profile::kFreshnessPolicies.size(),
          vehicle_profile::kDispatchTable.data(),
          vehicle_profile::kDispatchTable.size(),
          vehicle_profile::kMaxDecodeResults};
}

bool is_black(const local_argb::PixelFrame &frame) {
  return std::all_of(frame.begin(), frame.end(),
                     [](const local_argb::Rgb pixel) { return pixel == local_argb::kBlack; });
}

std::size_t count_color(const local_argb::PixelFrame &frame, const local_argb::Rgb color) {
  return static_cast<std::size_t>(std::count(frame.begin(), frame.end(), color));
}

[[maybe_unused]] bool same_pixels(const Pixels &left, const Pixels &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t frame = 0; frame < left.size(); ++frame) {
    if (!std::equal(left[frame].begin(), left[frame].end(), right[frame].begin()))
      return false;
  }
  return true;
}

[[maybe_unused]] Pixels replay_selected_profile() {
  const auto catalog = vehicle_profile::kCatalog;
  const auto *const rpm = catalog.find("vehicle.engine_rpm");
  const auto *const brake = catalog.find("vehicle.brake_pressed");
  REQUIRE(rpm != nullptr);
  REQUIRE(brake != nullptr);
  const auto *const rpm_dispatch = dispatch_for(rpm->id);
  const auto *const brake_dispatch = dispatch_for(brake->id);
  REQUIRE(rpm_dispatch != nullptr);
  REQUIRE(brake_dispatch != nullptr);
#if defined(REPLAY_SELECTED_PROFILE_EXPECT_ENGINE_ID)
  CHECK(rpm_dispatch->identifier == REPLAY_SELECTED_PROFILE_EXPECT_ENGINE_ID);
#endif
#if defined(REPLAY_SELECTED_PROFILE_EXPECT_BRAKE_ID)
  CHECK(brake_dispatch->identifier == REPLAY_SELECTED_PROFILE_EXPECT_BRAKE_ID);
#endif

  replay::ReplayClock clock{};
  RecordingPixelSink pixels{};
  replay::LocalArgbOutputStage stage{pixels, clock};
  RecordingObserver observer{};
  replay::ReplayController controller{{engine_frame(0, rpm_dispatch->identifier),
                                       brake_frame(10, brake_dispatch->identifier, true),
                                       brake_frame(11, brake_dispatch->identifier, false),
                                       engine_frame(100'002, rpm_dispatch->identifier)},
                                      clock,
                                      stage,
                                      replay::SignalObservers{&observer}};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(pixels.frames().size() == 1);
  CHECK(is_black(pixels.frames().back()));
  REQUIRE(observer.catalog_count == 1);
  REQUIRE(observer.catalog_size == 2);
  REQUIRE_FALSE(observer.order.empty());
  CHECK(observer.order.front() == "catalog");

  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.sample_polled_rules() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.sample_signals() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(observer.rpm_value == std::optional<float>{3250.0F});
  REQUIRE(pixels.frames().size() == 2);
  const auto &rpm_pixels = pixels.frames().back();
  CHECK_FALSE(is_black(rpm_pixels));
  CHECK(std::count_if(rpm_pixels.begin(), rpm_pixels.end(), [](const local_argb::Rgb pixel) {
          return pixel != local_argb::kBlack;
        }) == 50);
  CHECK(count_color(rpm_pixels, {0, 16, 16}) == 50);

  REQUIRE(clock.advance_to(10));
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(observer.brake_value == std::optional<bool>{true});
  CHECK(count_color(pixels.frames().back(), {16, 0, 0}) == 30);
  CHECK(count_color(pixels.frames().back(), {0, 16, 16}) == 20);
  const auto brake_reading =
      std::find(observer.order.begin(), observer.order.end(), "vehicle.brake_pressed");
  REQUIRE(brake_reading != observer.order.end());
  CHECK(observer.order.front() == "catalog");

  REQUIRE(clock.advance_to(11));
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(observer.brake_value == std::optional<bool>{false});
  CHECK(count_color(pixels.frames().back(), {0, 16, 16}) == 50);

  REQUIRE(clock.advance_to(100'001));
  REQUIRE(controller.process_timeout().input == replay::ReplayInputResult::Timeout);
  REQUIRE(controller.sample_polled_rules() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));

  REQUIRE(clock.advance_to(100'002));
  REQUIRE(controller.process_next_frame().input == replay::ReplayInputResult::Frame);
  REQUIRE(controller.sample_polled_rules() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.sample_signals() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(observer.rpm_value == std::optional<float>{3250.0F});
  CHECK(count_color(pixels.frames().back(), {0, 16, 16}) == 50);

  const auto fault = controller.process_source_fault();
  CHECK(fault.status == replay::ReplayControllerStatus::TelemetryFault);
  CHECK(fault.input == replay::ReplayInputResult::Fault);
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
  return pixels.frames();
}

} // namespace

TEST_CASE("empty selected profile stays empty and renders black through the real controller") {
  if (!vehicle_profile::kCatalog.empty())
    return;

  replay::ReplayClock clock{};
  RecordingPixelSink pixels{};
  replay::LocalArgbOutputStage stage{pixels, clock};
  RecordingObserver observer{};
  replay::ReplayController controller{{}, clock, stage, replay::SignalObservers{&observer}};

  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(observer.catalog_count == 1);
  CHECK(observer.catalog_size == 0);
  REQUIRE(observer.order.size() == 1);
  CHECK(observer.order.front() == "catalog");
  CHECK_FALSE(controller.next_frame_time().has_value());
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  REQUIRE_FALSE(pixels.frames().empty());
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.process_next_frame().input == replay::ReplayInputResult::EndOfStream);
  CHECK(controller.process_timeout().input == replay::ReplayInputResult::Timeout);
  REQUIRE(controller.tick_output() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);
  CHECK(is_black(pixels.frames().back()));
  REQUIRE(observer.order.size() == 1);
}

TEST_CASE("selected replay adapter processes and publishes the generated provider") {
  const auto *const brake = vehicle_profile::kCatalog.find("vehicle.brake_pressed");
  if (brake == nullptr)
    return;
  const auto *const brake_dispatch = dispatch_for(brake->id);
  REQUIRE(brake_dispatch != nullptr);

  replay::ReplayClock clock{};
  vehicle_profile_runtime::ReplayAdapter adapter{selected_tables(), clock};
  RecordedNotifications notices{};
  CallbackMutationProbe callback_probe{&adapter};
  const auto subscription = adapter.provider().subscribe(brake->id, record_notification, &notices);
  REQUIRE(subscription.ok());
  const auto callback_probe_subscription =
      adapter.provider().subscribe(brake->id, reject_callback_mutations, &callback_probe);
  REQUIRE(callback_probe_subscription.ok());
  REQUIRE(adapter.start() == vehicle_profile_runtime::ReplayAdapterStatus::Ok);
  REQUIRE(notices.count == 1);
  CHECK(notices.entries[0].initial);
  CHECK(notices.entries[0].id == brake->id);
  CHECK(notices.entries[0].current.availability == vehicle_signals::Availability::NoData);

  const auto frame = brake_frame(0, brake_dispatch->identifier, true);
  std::array<vehicle_profile_runtime::DecodeResult, 1> decoded{};
  std::size_t decoded_count = 0;
  brake_dispatch->decode(frame.frame, decoded.data(), decoded.size(), decoded_count);
  REQUIRE(decoded_count == 1);
  REQUIRE(decoded[0].valid);
  CHECK(decoded[0].value.as_boolean() == std::optional<bool>{true});
  CHECK(adapter.process(frame.frame) == vehicle_profile_runtime::ReplayAdapterStatus::Ok);
  REQUIRE(notices.count == 2);
  CHECK(notices.entries[1].id == brake->id);
  CHECK(notices.entries[1].current.availability == vehicle_signals::Availability::Fresh);
  CHECK_FALSE(notices.entries[1].became_unavailable);
  CHECK_FALSE(notices.entries[1].recovered);
  CHECK(callback_probe.fresh_notices == 1);
  CHECK(callback_probe.process_status ==
        vehicle_profile_runtime::ReplayAdapterStatus::InvalidState);
  CHECK(callback_probe.service_status ==
        vehicle_profile_runtime::ReplayAdapterStatus::InvalidState);
  CHECK(callback_probe.fault_status == vehicle_profile_runtime::ReplayAdapterStatus::InvalidState);
  CHECK(callback_probe.stop_status == vehicle_profile_runtime::ReplayAdapterStatus::InvalidState);
  CHECK(callback_probe.availability_after_attempt == vehicle_signals::Availability::Fresh);
  const auto reading = adapter.provider().read(brake->id);
  REQUIRE(reading.ok());
  REQUIRE(reading.value->value.has_value());
  CHECK(reading.value->value->as_boolean() == std::optional<bool>{true});

  REQUIRE(clock.advance_to(100'001));
  CHECK(adapter.service() == vehicle_profile_runtime::ReplayAdapterStatus::Ok);
  REQUIRE(notices.count == 3);
  CHECK(notices.entries[2].id == brake->id);
  CHECK(notices.entries[2].current.availability == vehicle_signals::Availability::Stale);
  CHECK(notices.entries[2].became_unavailable);
  CHECK_FALSE(notices.entries[2].recovered);

  const auto recovery_frame = brake_frame(100'002, brake_dispatch->identifier, false);
  CHECK(adapter.process(recovery_frame.frame) == vehicle_profile_runtime::ReplayAdapterStatus::Ok);
  REQUIRE(notices.count == 4);
  CHECK(notices.entries[3].id == brake->id);
  CHECK(notices.entries[3].current.availability == vehicle_signals::Availability::Fresh);
  CHECK_FALSE(notices.entries[3].became_unavailable);
  CHECK(notices.entries[3].recovered);
  CHECK(callback_probe.fresh_notices == 2);
  CHECK(callback_probe.stop_status == vehicle_profile_runtime::ReplayAdapterStatus::InvalidState);
  CHECK(callback_probe.availability_after_attempt == vehicle_signals::Availability::Fresh);

  CHECK(adapter.stop() == vehicle_profile_runtime::ReplayAdapterStatus::Ok);
  REQUIRE(notices.count == 5);
  CHECK(notices.entries[4].id == brake->id);
  CHECK(notices.entries[4].current.availability == vehicle_signals::Availability::Unavailable);
  CHECK(notices.entries[4].became_unavailable);
  CHECK_FALSE(notices.entries[4].recovered);
  const auto stopped = adapter.provider().read(brake->id);
  REQUIRE(stopped.ok());
  CHECK(stopped.value->availability == vehicle_signals::Availability::NoData);
}

TEST_CASE(
    "populated selected profile drives expected values, pixels and recovery deterministically") {
#if defined(REPLAY_SELECTED_PROFILE_EXPECT_ENGINE_ID) &&                                           \
    defined(REPLAY_SELECTED_PROFILE_EXPECT_BRAKE_ID)
  REQUIRE_FALSE(vehicle_profile::kCatalog.empty());
  REQUIRE(vehicle_profile::kCatalog.find("vehicle.engine_rpm") != nullptr);
  REQUIRE(vehicle_profile::kCatalog.find("vehicle.brake_pressed") != nullptr);
  CHECK(same_pixels(replay_selected_profile(), replay_selected_profile()));
#endif
}
