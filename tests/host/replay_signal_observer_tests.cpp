#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/scheduler.hpp"
#include "replay/signal_observer.hpp"
#include "replay/signal_record_output.hpp"
#include "signal_observer_fan_out.hpp"

namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

struct ObservedReading {
  vehicle_core::MonotonicTimestamp time_us{0};
  std::string key{};
  SignalReading reading{};
  const SignalMetadata *metadata{nullptr};
};

// Records every observer call in arrival order. It only sees vehicle_signals
// types, like any real observer.
class RecordingObserver final : public replay::SignalObserver {
public:
  void on_catalog(const SignalCatalogView catalog) noexcept override {
    ++catalog_calls_;
    catalog_ = catalog;
    catalog_before_readings_ = readings_.empty();
  }

  void on_reading(const vehicle_core::MonotonicTimestamp time_us, const SignalMetadata &signal,
                  const SignalReading &reading) noexcept override {
    readings_.push_back({time_us, std::string{signal.key}, reading, &signal});
  }

  [[nodiscard]] std::size_t catalog_calls() const noexcept { return catalog_calls_; }
  [[nodiscard]] SignalCatalogView catalog() const noexcept { return catalog_; }
  [[nodiscard]] bool catalog_before_readings() const noexcept { return catalog_before_readings_; }
  [[nodiscard]] const std::vector<ObservedReading> &readings() const noexcept { return readings_; }

  [[nodiscard]] std::vector<ObservedReading> readings_of(const std::string_view key) const {
    std::vector<ObservedReading> matching;
    std::copy_if(readings_.begin(), readings_.end(), std::back_inserter(matching),
                 [key](const ObservedReading &observed) { return observed.key == key; });
    return matching;
  }

private:
  std::size_t catalog_calls_{0};
  SignalCatalogView catalog_{};
  bool catalog_before_readings_{false};
  std::vector<ObservedReading> readings_{};
};

class RecordingEvents final : public replay::ReplayEventSink {
public:
  void record(const replay::ReplayEvent event) noexcept override { events_.push_back(event); }
  [[nodiscard]] const std::vector<replay::ReplayEvent> &events() const noexcept { return events_; }

private:
  std::vector<replay::ReplayEvent> events_{};
};

gvret::TimedCanFrame timed_frame(const vehicle_core::MonotonicTimestamp time_us,
                                 const std::uint32_t identifier,
                                 const std::initializer_list<std::uint8_t> bytes) {
  gvret::TimedCanFrame frame{};
  frame.relative_time_us = time_us;
  frame.frame.timestamp_us = time_us;
  frame.frame.identifier = identifier;
  frame.frame.dlc = static_cast<std::uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), frame.frame.data.begin());
  return frame;
}

// 0x32c8 / 4 = 3250 rpm.
gvret::TimedCanFrame rpm_3250(const vehicle_core::MonotonicTimestamp time_us) {
  return timed_frame(time_us, 0x202, {0x32, 0xc8, 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame left_turn(const vehicle_core::MonotonicTimestamp time_us) {
  return timed_frame(time_us, 0x091, {0, 0x20, 0, 0, 0, 0, 0, 0});
}

std::vector<gvret::TimedCanFrame> rpm_then_left_turn() { return {rpm_3250(0), left_turn(300'000)}; }

std::vector<vehicle_core::MonotonicTimestamp>
times_of(const std::vector<ObservedReading> &readings) {
  std::vector<vehicle_core::MonotonicTimestamp> times;
  for (const auto &observed : readings)
    times.push_back(observed.time_us);
  return times;
}

std::string choice_key(const ObservedReading &observed) {
  REQUIRE(observed.reading.value.has_value());
  const auto *choice = observed.metadata->find_choice(*observed.reading.value->as_enumeration());
  REQUIRE(choice != nullptr);
  return std::string{choice->key};
}

bool same_reading(const SignalReading &left, const SignalReading &right) {
  return left.value == right.value && left.availability == right.availability &&
         left.validation == right.validation;
}

std::string render_pixels(const replay::SignalObservers *observers) {
  replay::ReplayClock clock;
  std::ostringstream stream;
  replay::JsonlPixelFrameSink pixels{clock, stream};
  replay::LocalArgbOutputStage stage{pixels};
  const auto result =
      observers == nullptr
          ? replay::run_replay(rpm_then_left_turn(), clock, stage, {600'000})
          : replay::run_replay(rpm_then_left_turn(), clock, stage, *observers, {600'000});
  REQUIRE(result.ok());
  REQUIRE(pixels.write_end());
  return stream.str();
}

std::string render_signal_records() {
  replay::ReplayClock clock;
  std::ostringstream pixel_stream;
  std::ostringstream signal_stream;
  replay::JsonlPixelFrameSink pixels{clock, pixel_stream};
  replay::LocalArgbOutputStage stage{pixels};
  replay::JsonlSignalRecordWriter signals{signal_stream};
  const auto result = replay::run_replay(rpm_then_left_turn(), clock, stage, {&signals}, {600'000});
  REQUIRE(result.ok());
  REQUIRE(signals.good());
  return signal_stream.str();
}

// A provider double over a synthetic catalog. It lets the fan-out be tested
// without the Mazda provider and hands back exactly the readings it is given.
constexpr SignalEnumChoice kModeChoices[] = {{0, "idle"}, {3, "busy"}};

constexpr SignalMetadata kFakeCatalog[] = {
    {SignalId{7}, "test.level", SignalType::Number, SignalUnit::KilometresPerHour,
     ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0},
    {SignalId{8}, "test.switch", SignalType::Boolean, SignalUnit::None, ValidationStatus::Observed,
     SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
    {SignalId{9}, "test.mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read | SignalCapability::Notify, kModeChoices, 2},
};

class FakeProvider final : public vehicle_signals::SignalProvider {
public:
  struct Subscription {
    SignalId id{};
    vehicle_signals::SignalCallback callback{nullptr};
    void *context{nullptr};
    bool active{false};
  };

  [[nodiscard]] SignalCatalogView catalog() const noexcept override { return kFakeCatalog; }

  [[nodiscard]] vehicle_signals::SignalResult<SignalReading>
  read(const SignalId id) const noexcept override {
    ++reads_;
    read_ids_.push_back(id);
    if (fail_reads_)
      return vehicle_signals::SignalResult<SignalReading>::failure(
          vehicle_signals::SignalStatus::Faulted);
    return vehicle_signals::SignalResult<SignalReading>::success(level_reading_);
  }

  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(const SignalId id, const vehicle_signals::SignalCallback callback,
            void *context) noexcept override {
    if (fail_subscribe_id_ == id)
      return vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>::failure(
          vehicle_signals::SignalStatus::CapacityExceeded);
    subscriptions_.push_back({id, callback, context, true});
    return vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>::success(
        vehicle_signals::SignalSubscription::from_provider_bits(subscriptions_.size()));
  }

  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(const vehicle_signals::SignalSubscription subscription) noexcept override {
    const auto index = subscription.provider_bits() - 1;
    if (index >= subscriptions_.size() || !subscriptions_[index].active)
      return vehicle_signals::SignalStatusResult::failure(
          vehicle_signals::SignalStatus::InvalidSubscription);
    subscriptions_[index].active = false;
    return vehicle_signals::SignalStatusResult::success();
  }

  void notify(const SignalId id, const SignalReading &reading) const {
    for (const auto &subscription : subscriptions_) {
      if (subscription.active && subscription.id == id)
        subscription.callback(subscription.context, {id, reading, false, false, false, false});
    }
  }

  [[nodiscard]] std::size_t active_subscriptions() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(subscriptions_.begin(), subscriptions_.end(),
                      [](const Subscription &subscription) { return subscription.active; }));
  }
  [[nodiscard]] const std::vector<Subscription> &subscriptions() const noexcept {
    return subscriptions_;
  }
  [[nodiscard]] const std::vector<SignalId> &read_ids() const noexcept { return read_ids_; }

  void set_level_reading(const SignalReading &reading) noexcept { level_reading_ = reading; }
  void fail_reads() noexcept { fail_reads_ = true; }
  void fail_subscribe(const SignalId id) noexcept { fail_subscribe_id_ = id; }

private:
  std::vector<Subscription> subscriptions_{};
  mutable std::size_t reads_{0};
  mutable std::vector<SignalId> read_ids_{};
  SignalReading level_reading_{};
  bool fail_reads_{false};
  SignalId fail_subscribe_id_{};
};

// Forwards to a real stage but fails every tick, so ReplayController::start()
// fails after telemetry has started.
class FailingTickStage final : public replay::OutputStage {
public:
  explicit FailingTickStage(replay::OutputStage &inner) noexcept : inner_(&inner) {}

  [[nodiscard]] bool configure(action_engine::ActionEngine &engine) noexcept override {
    return inner_->configure(engine);
  }
  [[nodiscard]] bool start(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    return inner_->start(now_us);
  }
  [[nodiscard]] bool tick(vehicle_core::MonotonicTimestamp) noexcept override { return false; }
  [[nodiscard]] bool fail_off(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    return inner_->fail_off(now_us);
  }
  [[nodiscard]] bool stop(const vehicle_core::MonotonicTimestamp now_us) noexcept override {
    return inner_->stop(now_us);
  }
  [[nodiscard]] vehicle_core::Microseconds tick_period_us() const noexcept override {
    return inner_->tick_period_us();
  }

private:
  replay::OutputStage *inner_;
};

std::size_t line_index(const std::string &lines, const std::string_view needle) {
  const auto position = lines.find(needle);
  REQUIRE(position != std::string::npos);
  return static_cast<std::size_t>(std::count(lines.begin(), lines.begin() + position, '\n'));
}

} // namespace

TEST_CASE("fan-out attach subscribes to every notify signal without publishing the catalog") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};

  REQUIRE(fan_out.attach());

  CHECK(fan_out.attached());
  CHECK(observer.catalog_calls() == 0);
  REQUIRE(provider.subscriptions().size() == 2);
  CHECK(provider.subscriptions()[0].id == SignalId{8});
  CHECK(provider.subscriptions()[1].id == SignalId{9});
}

TEST_CASE("fan-out open publishes the catalog once, then the readings held since attach") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());
  const SignalReading on{SignalValue::boolean(true), Availability::Fresh,
                         ValidationStatus::Observed};
  const SignalReading busy{SignalValue::enumeration(3), Availability::Fresh,
                           ValidationStatus::Reference};
  provider.notify(SignalId{8}, on);
  REQUIRE(clock.advance_to(5));
  provider.notify(SignalId{9}, busy);
  REQUIRE(observer.readings().empty());

  fan_out.open();
  fan_out.open();

  CHECK(observer.catalog_calls() == 1);
  CHECK(observer.catalog().begin() == provider.catalog().begin());
  CHECK(observer.catalog_before_readings());
  REQUIRE(observer.readings().size() == 2);
  CHECK(observer.readings()[0].key == "test.switch");
  CHECK(observer.readings()[0].time_us == 0);
  CHECK(same_reading(observer.readings()[0].reading, on));
  CHECK(observer.readings()[1].key == "test.mode");
  CHECK(observer.readings()[1].time_us == 5);
  CHECK(same_reading(observer.readings()[1].reading, busy));
}

TEST_CASE("fan-out detach before open drops the held readings unseen") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());
  provider.notify(SignalId{8},
                  {SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Observed});

  REQUIRE(fan_out.detach());
  fan_out.open();

  CHECK(observer.catalog_calls() == 0);
  CHECK(observer.readings().empty());
}

TEST_CASE("fan-out forwards a notified reading unchanged at the replay time") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver first;
  RecordingObserver second;
  replay::SignalObserverFanOut fan_out{provider, clock, {&first, &second}};
  REQUIRE(fan_out.attach());
  fan_out.open();
  REQUIRE(clock.advance_to(42'000));

  const SignalReading stale{SignalValue::enumeration(3), Availability::Stale,
                            ValidationStatus::Observed};
  provider.notify(SignalId{9}, stale);

  for (const auto *observer : {&first, &second}) {
    REQUIRE(observer->readings().size() == 1);
    const auto &observed = observer->readings().front();
    CHECK(observed.time_us == 42'000);
    CHECK(observed.key == "test.mode");
    CHECK(observed.metadata == &kFakeCatalog[2]);
    CHECK(same_reading(observed.reading, stale));
  }
}

TEST_CASE("fan-out samples only polled signals and passes availability through") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());
  fan_out.open();
  REQUIRE(clock.advance_to(100'000));

  for (const auto availability : {Availability::NoData, Availability::Fresh, Availability::Stale,
                                  Availability::FreshnessUnverified, Availability::Unavailable}) {
    const SignalReading reading{availability == Availability::NoData
                                    ? std::nullopt
                                    : std::optional<SignalValue>{SignalValue::number(12.5F)},
                                availability, ValidationStatus::Confirmed};
    provider.set_level_reading(reading);
    REQUIRE(fan_out.sample());
    REQUIRE_FALSE(observer.readings().empty());
    CHECK(observer.readings().back().key == "test.level");
    CHECK(observer.readings().back().time_us == 100'000);
    CHECK(same_reading(observer.readings().back().reading, reading));
  }
  CHECK(observer.readings().size() == 5);
  CHECK(std::all_of(provider.read_ids().begin(), provider.read_ids().end(),
                    [](const SignalId id) { return id == SignalId{7}; }));
}

TEST_CASE("fan-out reports a failed read") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());
  fan_out.open();
  provider.fail_reads();

  CHECK_FALSE(fan_out.sample());
  CHECK(observer.readings().empty());
}

TEST_CASE("fan-out detach releases every subscription") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
  REQUIRE(fan_out.attach());

  CHECK(fan_out.detach());
  CHECK_FALSE(fan_out.attached());
  CHECK(provider.active_subscriptions() == 0);
}

TEST_CASE("fan-out rolls back its subscriptions when one subscribe fails") {
  FakeProvider provider;
  provider.fail_subscribe(SignalId{9});
  replay::ReplayClock clock;
  RecordingObserver observer;
  replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};

  CHECK_FALSE(fan_out.attach());
  CHECK_FALSE(fan_out.attached());
  CHECK(provider.active_subscriptions() == 0);
}

TEST_CASE("fan-out detach releases every subscription on the Mazda provider") {
  mazda::VehicleTelemetry telemetry;
  mazda::MazdaSignalProvider provider{telemetry};
  replay::ReplayClock clock;
  RecordingObserver observer;

  // Without detach, the facade's fixed subscriber capacity runs out quickly.
  std::vector<std::unique_ptr<replay::SignalObserverFanOut>> leaked;
  bool exhausted = false;
  for (int attempt = 0; attempt < 32 && !exhausted; ++attempt) {
    leaked.push_back(std::make_unique<replay::SignalObserverFanOut>(
        provider, clock, replay::SignalObservers{&observer}));
    exhausted = !leaked.back()->attach();
  }
  REQUIRE(exhausted);
  for (const auto &fan_out : leaked)
    if (fan_out->attached())
      REQUIRE(fan_out->detach());

  for (int cycle = 0; cycle < 32; ++cycle) {
    replay::SignalObserverFanOut fan_out{provider, clock, {&observer}};
    REQUIRE(fan_out.attach());
    REQUIRE(fan_out.detach());
  }
}

TEST_CASE("fan-out with no observers neither subscribes nor reads") {
  FakeProvider provider;
  replay::ReplayClock clock;
  replay::SignalObserverFanOut fan_out{provider, clock, {}};

  REQUIRE(fan_out.attach());
  REQUIRE(fan_out.sample());
  CHECK(provider.subscriptions().empty());
  CHECK(provider.read_ids().empty());
}

TEST_CASE("fan-out rejects a null observer") {
  FakeProvider provider;
  replay::ReplayClock clock;
  RecordingObserver observer;

  CHECK(replay::SignalObserverFanOut{provider, clock, {&observer}}.configured());
  CHECK_FALSE(replay::SignalObserverFanOut{provider, clock, {&observer, nullptr}}.configured());
}

TEST_CASE("replay observer receives the production catalog once before any reading") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;

  const auto result =
      replay::run_replay(rpm_then_left_turn(), clock, stage, {&observer}, {600'000});

  REQUIRE(result.ok());
  CHECK(observer.catalog_calls() == 1);
  CHECK(observer.catalog_before_readings());
  CHECK(observer.catalog().size() == 18);
  CHECK(observer.catalog().find("vehicle.engine_rpm") != nullptr);
  CHECK(observer.catalog().find("vehicle.turn_state") != nullptr);
}

TEST_CASE("replay observer receives a notified turn at its CAN frame time") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;

  const auto result =
      replay::run_replay(rpm_then_left_turn(), clock, stage, {&observer}, {600'000});

  REQUIRE(result.ok());
  const auto turns = observer.readings_of("vehicle.turn_state");
  REQUIRE_FALSE(turns.empty());
  CHECK(turns.front().time_us == 0);
  const auto left = std::find_if(turns.begin(), turns.end(), [](const ObservedReading &observed) {
    return observed.reading.value.has_value() &&
           observed.metadata->find_choice(*observed.reading.value->as_enumeration()) != nullptr &&
           choice_key(observed) == "left";
  });
  REQUIRE(left != turns.end());
  CHECK(left->time_us == 300'000);
  CHECK(left->reading.availability == Availability::Fresh);
}

TEST_CASE("a sparse notified turn goes stale through a timeout publication") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;

  const auto result = replay::run_replay({left_turn(0)}, clock, stage, {&observer}, {600'000});

  REQUIRE(result.ok());
  const auto turns = observer.readings_of("vehicle.turn_state");
  REQUIRE_FALSE(turns.empty());
  CHECK(turns.back().reading.availability == Availability::Stale);
  CHECK(turns.back().time_us > 0);
  CHECK(turns.back().time_us % mazda::kDefaultAvailabilityServiceTargetUs == 0);
}

TEST_CASE("polled signals are sampled at the polled-rule cadence by default") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;

  const auto result =
      replay::run_replay(rpm_then_left_turn(), clock, stage, {&observer}, {500'000});

  REQUIRE(result.ok());
  CHECK(result.signal_samples == 6);
  const std::vector<vehicle_core::MonotonicTimestamp> expected{0,       100'000, 200'000,
                                                               300'000, 400'000, 500'000};
  CHECK(times_of(observer.readings_of("vehicle.engine_rpm")) == expected);
  CHECK(times_of(observer.readings_of("vehicle.speed_kph")) == expected);

  const auto rpm = observer.readings_of("vehicle.engine_rpm");
  REQUIRE(rpm.front().reading.value.has_value());
  CHECK(*rpm.front().reading.value->as_number() == doctest::Approx(3250.0F));
  CHECK(rpm.front().reading.availability == Availability::FreshnessUnverified);
}

TEST_CASE("the signal sample cadence is configurable") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;
  replay::ReplayScheduleOptions options{500'000};
  options.signal_sample_period_us = 250'000;

  const auto result = replay::run_replay(rpm_then_left_turn(), clock, stage, {&observer}, options);

  REQUIRE(result.ok());
  CHECK(result.signal_samples == 3);
  CHECK(result.polled_samples == 6);
  const std::vector<vehicle_core::MonotonicTimestamp> expected{0, 250'000, 500'000};
  CHECK(times_of(observer.readings_of("vehicle.engine_rpm")) == expected);
}

TEST_CASE("signal samples run after polled rules and before the output tick") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;
  RecordingEvents events;

  const auto result = replay::run_replay({rpm_3250(0), left_turn(100'000)}, clock, stage,
                                         {&observer}, {100'000}, &events);

  REQUIRE(result.ok());
  using Kind = replay::ReplayEventKind;
  const std::vector<replay::ReplayEvent> expected_prefix{
      {0, Kind::Frame},      {0, Kind::Poll},         {0, Kind::SignalSample},
      {0, Kind::OutputTick}, {10'000, Kind::Timeout}, {10'000, Kind::OutputTick},
  };
  REQUIRE(events.events().size() > expected_prefix.size());
  for (std::size_t index = 0; index < expected_prefix.size(); ++index) {
    CHECK(events.events()[index].time_us == expected_prefix[index].time_us);
    CHECK(events.events()[index].kind == expected_prefix[index].kind);
  }
  const std::vector<replay::ReplayEvent> expected_tail{
      {100'000, Kind::Frame}, {100'000, Kind::EndOfStream},  {100'000, Kind::Timeout},
      {100'000, Kind::Poll},  {100'000, Kind::SignalSample}, {100'000, Kind::OutputTick},
  };
  REQUIRE(events.events().size() >= expected_tail.size());
  const auto tail_start = events.events().size() - expected_tail.size();
  for (std::size_t index = 0; index < expected_tail.size(); ++index) {
    CHECK(events.events()[tail_start + index].time_us == expected_tail[index].time_us);
    CHECK(events.events()[tail_start + index].kind == expected_tail[index].kind);
  }
}

TEST_CASE("no signal sample is scheduled without an observer") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingEvents events;

  const auto result = replay::run_replay(rpm_then_left_turn(), clock, stage,
                                         replay::SignalObservers{}, {500'000}, &events);

  REQUIRE(result.ok());
  CHECK(result.signal_samples == 0);
  CHECK(std::none_of(events.events().begin(), events.events().end(),
                     [](const replay::ReplayEvent event) {
                       return event.kind == replay::ReplayEventKind::SignalSample;
                     }));
}

TEST_CASE("invalid signal sample options are rejected before the observer is attached") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingObserver observer;

  replay::ReplayScheduleOptions zero{500'000};
  zero.signal_sample_period_us = 0;
  CHECK(replay::run_replay({}, clock, stage, {&observer}, zero).status ==
        replay::ReplayScheduleStatus::InvalidOptions);

  replay::ReplayScheduleOptions dense{3'000'000};
  dense.signal_sample_period_us = 1;
  CHECK(replay::run_replay({}, clock, stage, {&observer}, dense).status ==
        replay::ReplayScheduleStatus::InvalidOptions);
  CHECK(observer.catalog_calls() == 0);
  CHECK(pixels.frames().empty());
}

TEST_CASE("a failed controller start delivers nothing and releases the observer") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage inner{pixels};
  FailingTickStage stage{inner};
  RecordingObserver observer;
  replay::ReplayController controller{rpm_then_left_turn(), clock, stage, {&observer}};

  CHECK(controller.start() == replay::ReplayControllerStatus::OutputFault);

  CHECK(observer.catalog_calls() == 0);
  CHECK(observer.readings().empty());
  CHECK_FALSE(controller.running());
  // Stopped, not Failed: a start is only fully rolled back once telemetry is
  // quiescent and every observer subscription is released.
  CHECK(controller.stop() == replay::ReplayControllerStatus::InvalidState);
}

TEST_CASE("a stopped controller releases the observer and delivers nothing more") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingObserver observer;
  replay::ReplayController controller{rpm_then_left_turn(), clock, stage, {&observer}};
  REQUIRE(controller.start() == replay::ReplayControllerStatus::Ok);
  REQUIRE(controller.process_next_frame().ok());
  const auto delivered = observer.readings().size();

  CHECK(controller.stop() == replay::ReplayControllerStatus::Ok);

  CHECK_FALSE(controller.running());
  CHECK(controller.sample_signals() == replay::ReplayControllerStatus::InvalidState);
  CHECK(controller.stop() == replay::ReplayControllerStatus::InvalidState);
  CHECK(observer.readings().size() == delivered);
}

TEST_CASE("a polled signal without frames becomes unavailable on the production path") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink ignored{clock};
  replay::LocalArgbOutputStage stage{ignored};
  RecordingObserver observer;

  const auto result = replay::run_replay({rpm_3250(0)}, clock, stage, {&observer}, {1'500'000});

  REQUIRE(result.ok());
  const auto rpm = observer.readings_of("vehicle.engine_rpm");
  REQUIRE_FALSE(rpm.empty());
  CHECK(rpm.front().reading.availability == Availability::FreshnessUnverified);
  CHECK(rpm.back().reading.availability == Availability::Unavailable);
  CHECK(rpm.back().reading.validation == ValidationStatus::Confirmed);
  // Polled Mazda signals are freshness-unverified, so none ever reads Stale.
  CHECK(std::none_of(rpm.begin(), rpm.end(), [](const ObservedReading &observed) {
    return observed.reading.availability == Availability::Stale;
  }));
}

TEST_CASE("a zero signal sample period is rejected even without observers") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  replay::ReplayScheduleOptions zero{500'000};
  zero.signal_sample_period_us = 0;

  CHECK(replay::run_replay({}, clock, stage, zero).status ==
        replay::ReplayScheduleStatus::InvalidOptions);
  CHECK(pixels.frames().empty());
}

TEST_CASE("a null observer fails controller configuration") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};

  const auto result = replay::run_replay({}, clock, stage, {nullptr}, {100'000});

  CHECK(result.status == replay::ReplayScheduleStatus::ControllerFailure);
  CHECK(result.controller_status == replay::ReplayControllerStatus::ConfigurationFailed);
}

TEST_CASE("sampling signals before start is an invalid state") {
  replay::ReplayClock clock;
  replay::TimestampedPixelFrameSink pixels{clock};
  replay::LocalArgbOutputStage stage{pixels};
  RecordingObserver observer;
  replay::ReplayController controller{{}, clock, stage, {&observer}};

  CHECK(controller.sample_signals() == replay::ReplayControllerStatus::InvalidState);
  CHECK(observer.catalog_calls() == 0);
}

TEST_CASE("pixel output is byte-identical with and without observers") {
  const auto baseline = render_pixels(nullptr);
  const replay::SignalObservers none{};
  RecordingObserver observer;
  const replay::SignalObservers one{&observer};

  CHECK(render_pixels(&none) == baseline);
  CHECK(render_pixels(&one) == baseline);
  CHECK_FALSE(observer.readings().empty());
}

TEST_CASE("signal records are byte-identical across replays") {
  const auto first = render_signal_records();
  const auto second = render_signal_records();

  CHECK_FALSE(first.empty());
  CHECK(first == second);
  CHECK(first.find("{\"type\":\"signal\",\"timestamp_us\":300000,\"signal\":\"vehicle.turn_state\","
                   "\"value\":\"left\",\"unit\":null,\"freshness\":\"fresh\","
                   "\"availability\":\"fresh\"") != std::string::npos);
}

TEST_CASE("a notified record precedes the same-timestamp signal sample records") {
  const auto records = render_signal_records();

  const auto turn =
      line_index(records, "\"timestamp_us\":300000,\"signal\":\"vehicle.turn_state\"");
  const auto rpm = line_index(records, "\"timestamp_us\":300000,\"signal\":\"vehicle.engine_rpm\"");
  const auto speed =
      line_index(records, "\"timestamp_us\":300000,\"signal\":\"vehicle.speed_kph\"");
  CHECK(turn < rpm);
  CHECK(rpm < speed);
}

TEST_CASE("signal records spell out value, unit, freshness and availability") {
  std::ostringstream stream;
  replay::JsonlSignalRecordWriter writer{stream};
  writer.on_catalog(kFakeCatalog);

  writer.on_reading(
      0, kFakeCatalog[0],
      {SignalValue::number(12.5F), Availability::FreshnessUnverified, ValidationStatus::Confirmed});
  writer.on_reading(10, kFakeCatalog[1],
                    {SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Observed});
  writer.on_reading(
      20, kFakeCatalog[2],
      {SignalValue::enumeration(3), Availability::Stale, ValidationStatus::Reference});
  writer.on_reading(
      30, kFakeCatalog[2],
      {SignalValue::enumeration(5), Availability::Fresh, ValidationStatus::Reference});
  writer.on_reading(40, kFakeCatalog[0],
                    {std::nullopt, Availability::NoData, ValidationStatus::Confirmed});
  writer.on_reading(50, kFakeCatalog[1],
                    {std::nullopt, Availability::Unavailable, ValidationStatus::Observed});
  writer.on_reading(60, kFakeCatalog[0],
                    {SignalValue::number(std::numeric_limits<float>::infinity()),
                     Availability::Fresh, ValidationStatus::Confirmed});
  writer.on_reading(
      70, kFakeCatalog[0],
      {SignalValue::number(3250.0F), Availability::Fresh, ValidationStatus::Confirmed});

  REQUIRE(writer.good());
  CHECK(stream.str() ==
        "{\"type\":\"signal\",\"timestamp_us\":0,\"signal\":\"test.level\",\"value\":12.5,"
        "\"unit\":\"km/h\",\"freshness\":\"unverified\",\"availability\":\"freshness_unverified\","
        "\"validation\":\"confirmed\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":10,\"signal\":\"test.switch\",\"value\":true,"
        "\"unit\":null,\"freshness\":\"fresh\",\"availability\":\"fresh\","
        "\"validation\":\"observed\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":20,\"signal\":\"test.mode\",\"value\":\"busy\","
        "\"unit\":null,\"freshness\":\"stale\",\"availability\":\"stale\","
        "\"validation\":\"reference\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":30,\"signal\":\"test.mode\",\"value\":5,"
        "\"unit\":null,\"freshness\":\"fresh\",\"availability\":\"fresh\","
        "\"validation\":\"reference\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":40,\"signal\":\"test.level\",\"value\":null,"
        "\"unit\":\"km/h\",\"freshness\":null,\"availability\":\"no_data\","
        "\"validation\":\"confirmed\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":50,\"signal\":\"test.switch\",\"value\":null,"
        "\"unit\":null,\"freshness\":null,\"availability\":\"unavailable\","
        "\"validation\":\"observed\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":60,\"signal\":\"test.level\",\"value\":null,"
        "\"unit\":\"km/h\",\"freshness\":\"fresh\",\"availability\":\"fresh\","
        "\"validation\":\"confirmed\"}\n"
        "{\"type\":\"signal\",\"timestamp_us\":70,\"signal\":\"test.level\",\"value\":3250,"
        "\"unit\":\"km/h\",\"freshness\":\"fresh\",\"availability\":\"fresh\","
        "\"validation\":\"confirmed\"}\n");
}

TEST_CASE("signal record keys are JSON-escaped") {
  constexpr SignalMetadata quoted{SignalId{1},
                                  "odd\"key\\\n",
                                  SignalType::Boolean,
                                  SignalUnit::None,
                                  ValidationStatus::Reference,
                                  SignalCapability::Read,
                                  nullptr,
                                  0};
  std::ostringstream stream;
  replay::JsonlSignalRecordWriter writer{stream};

  writer.on_reading(
      1, quoted, {SignalValue::boolean(false), Availability::Fresh, ValidationStatus::Reference});

  CHECK(stream.str() == "{\"type\":\"signal\",\"timestamp_us\":1,\"signal\":\"odd\\\"key\\\\\\n\","
                        "\"value\":false,\"unit\":null,\"freshness\":\"fresh\","
                        "\"availability\":\"fresh\",\"validation\":\"reference\"}\n");
}
