#include "mazda/signal_catalog.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <tuple>
#include <type_traits>

namespace {

class TestClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp value) noexcept {
    now_us_.store(value, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{1'000};
};

class TestLightingSink final : public mazda::internal::LightingSink {
public:
  [[nodiscard]] bool publish(const mazda::LightingUpdate &) noexcept override { return true; }
};

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                std::initializer_list<std::uint8_t> bytes) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = static_cast<std::uint8_t>(bytes.size());
  std::size_t index = 0;
  for (const auto byte : bytes)
    result.data[index++] = byte;
  return result;
}

template <typename Predicate>
bool wait_for(Predicate predicate,
              const std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

template <typename T> vehicle_signals::SignalValue generic_value(const T value) noexcept {
  using Value = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<Value, bool>) {
    return vehicle_signals::SignalValue::boolean(value);
  } else if constexpr (std::is_enum_v<Value>) {
    return vehicle_signals::SignalValue::enumeration(static_cast<std::int32_t>(value));
  } else {
    return vehicle_signals::SignalValue::number(static_cast<double>(value));
  }
}

template <typename T>
bool same_reading(const vehicle_signals::SignalReadResult &generic,
                  const vehicle_core::Reading<T> &typed) noexcept {
  if (generic.status != vehicle_signals::SignalStatus::Ok ||
      generic.reading.availability != typed.availability ||
      generic.reading.validation != typed.validation ||
      generic.reading.value.has_value() != typed.value.has_value())
    return false;
  return !typed.value.has_value() || generic.reading.value == generic_value(*typed.value);
}

template <typename Descriptor>
bool notification_read_matches(mazda::internal::VehicleTelemetryService &service,
                               mazda::MazdaSignalProvider &provider,
                               const Descriptor &descriptor) noexcept {
  if (!descriptor.signal_id.valid())
    return true;
  const auto typed = (service.*descriptor.channel).current();
  return same_reading(provider.read(descriptor.signal_id), typed);
}

bool notification_reads_match(mazda::internal::VehicleTelemetryService &service,
                              mazda::MazdaSignalProvider &provider) noexcept {
  bool match = true;
  std::apply(
      [&service, &provider, &match](const auto &...descriptor) {
        ((match = notification_read_matches(service, provider, descriptor) && match), ...);
      },
      mazda::internal::VehicleTelemetryService::notification_descriptors());
  return match;
}

template <typename Descriptor>
bool polling_read_matches(mazda::internal::VehicleTelemetryService &service,
                          mazda::MazdaSignalProvider &provider,
                          const Descriptor &descriptor) noexcept {
  if (!descriptor.signal_id.valid())
    return true;
  return same_reading(provider.read(descriptor.signal_id),
                      service.read_polling_descriptor(descriptor));
}

bool polling_reads_match(mazda::internal::VehicleTelemetryService &service,
                         mazda::MazdaSignalProvider &provider) noexcept {
  bool match = true;
  std::apply(
      [&service, &provider, &match](const auto &...descriptor) {
        ((match = polling_read_matches(service, provider, descriptor) && match), ...);
      },
      mazda::internal::VehicleTelemetryService::polling_descriptors());
  return match;
}

bool inject_and_wait(mazda::internal::VehicleTelemetryService &service,
                     mazda::internal::HostRuntimeSource &source,
                     const vehicle_core::RawCanFrame &input,
                     const std::uint64_t processed_count) noexcept {
  return source.inject(input) == mazda::ResultCode::Ok && wait_for([&service, processed_count] {
           return service.diagnostics().acquisition.frames_processed >= processed_count;
         });
}

} // namespace

int main() {
  TestClock clock;
  mazda::internal::HostRuntimeSource source;
  TestLightingSink lighting;
  mazda::TelemetryConfig config{};
  config.transport_silence_timeout_us = 10'000'000;
  mazda::internal::VehicleTelemetryService service{clock, source, lighting, config};
  auto provider = mazda::internal::VehicleTelemetryAccess::for_host_test(service);

  const auto catalog = provider.catalog();
  if (catalog.size() != mazda::internal::signals::kSignalCount)
    return 1;
  if (!service.start().ok())
    return 2;

  if (!wait_for(
          [&service] { return service.diagnostics().lifecycle == mazda::LifecycleState::Running; }))
    return 3;

  // Valid IDs are readable before their first sample. This distinguishes
  // NoData from an invalid ID while retaining catalog validation metadata.
  for (const auto &metadata : catalog) {
    const auto result = provider.read(metadata.id);
    if (result.status != vehicle_signals::SignalStatus::Ok || result.reading.value.has_value() ||
        result.reading.availability != vehicle_core::Availability::NoData ||
        result.reading.validation != metadata.validation)
      return 4;
  }
  if (provider.read(vehicle_signals::SignalId{}).status !=
          vehicle_signals::SignalStatus::InvalidSignal ||
      provider.read(vehicle_signals::SignalId{19}).status !=
          vehicle_signals::SignalStatus::InvalidSignal)
    return 5;

  std::uint64_t expected_processed = 1;
  clock.set(1'000);
  if (!inject_and_wait(
          service, source,
          frame(mazda::candidate::kEngineDataId, 1'000, {0x09, 0x5b, 0x13, 0x88, 0, 0, 0, 0}),
          expected_processed++))
    return 6;
  clock.set(2'000);
  if (!inject_and_wait(service, source,
                       frame(mazda::candidate::kGearId, 2'000, {0x04, 0, 0, 0, 0x1c, 0, 0, 0}),
                       expected_processed++))
    return 7;
  clock.set(3'000);
  if (!inject_and_wait(service, source,
                       frame(mazda::candidate::kDoorsId, 3'000, {0, 0, 0, 0x40, 0x3d, 0, 0, 0}),
                       expected_processed++))
    return 8;
  clock.set(4'000);
  if (!inject_and_wait(service, source,
                       frame(mazda::candidate::kBlinkInfoId, 4'000, {0, 0, 0x0c, 0, 0x02, 0, 0, 0}),
                       expected_processed++))
    return 9;
  clock.set(5'000);
  if (!inject_and_wait(
          service, source,
          frame(mazda::candidate::kTurnSwitchId, 5'000, {0, 0x34, 0x10, 0, 0, 0, 0, 0}),
          expected_processed++))
    return 10;

  if (!wait_for([&service, &provider] {
        return polling_reads_match(service, provider) &&
               notification_reads_match(service, provider);
      }))
    return 11;

  const auto rpm = provider.read(mazda::internal::signals::kEngineRpm);
  const auto speed = provider.read(mazda::internal::signals::kSpeedKph);
  if (!rpm.reading.value.has_value() || !rpm.reading.value->is_number() ||
      rpm.reading.value->as_number() != std::optional<double>{598.75} ||
      rpm.reading.availability != vehicle_core::Availability::FreshnessUnverified ||
      rpm.reading.validation != vehicle_core::ValidationStatus::Confirmed)
    return 12;
  if (!speed.reading.value.has_value() || !speed.reading.value->is_number() ||
      speed.reading.value->as_number() != std::optional<double>{50.0} ||
      speed.reading.availability != vehicle_core::Availability::FreshnessUnverified ||
      speed.reading.validation != vehicle_core::ValidationStatus::Reference)
    return 13;

  const auto turn = provider.read(mazda::internal::signals::kTurnState);
  if (!turn.reading.value.has_value() || !turn.reading.value->is_enum() ||
      turn.reading.value->as_enum() !=
          std::optional<std::int32_t>{static_cast<std::int32_t>(mazda::TurnState::Hazard)} ||
      turn.reading.availability != vehicle_core::Availability::Fresh)
    return 14;

  // The fake clock is sampled on every generic read, so the turn/request
  // values become stale without a new publication. RPM remains unverified
  // because the reviewed source has no configured freshness timeout.
  clock.set(5'000 + mazda::kTurnFreshnessTimeoutUs + 1);
  const auto stale_turn = provider.read(mazda::internal::signals::kTurnState);
  if (stale_turn.status != vehicle_signals::SignalStatus::Ok ||
      stale_turn.reading.availability != vehicle_core::Availability::Stale ||
      stale_turn.reading.value != turn.reading.value)
    return 15;
  const auto unverified_rpm = provider.read(mazda::internal::signals::kEngineRpm);
  if (unverified_rpm.reading.availability != vehicle_core::Availability::FreshnessUnverified ||
      unverified_rpm.reading.value != rpm.reading.value)
    return 16;

  // A malformed frame faults the message health but leaves the last accepted
  // typed sample intact. Generic reads preserve that value and report it as
  // unavailable through the same publication evaluator.
  constexpr vehicle_core::MonotonicTimestamp malformed_time =
      5'000 + mazda::kTurnFreshnessTimeoutUs + 2;
  clock.set(malformed_time);
  if (!inject_and_wait(service, source,
                       frame(mazda::candidate::kEngineDataId, malformed_time, {0x09}),
                       expected_processed++))
    return 17;
  const auto malformed_rpm = provider.read(mazda::internal::signals::kEngineRpm);
  if (malformed_rpm.status != vehicle_signals::SignalStatus::Ok ||
      malformed_rpm.reading.value != rpm.reading.value ||
      malformed_rpm.reading.availability != vehicle_core::Availability::Unavailable)
    return 18;

  if (!service.stop().ok())
    return 19;
  const auto stopped = provider.read(mazda::internal::signals::kEngineRpm);
  if (stopped.status != vehicle_signals::SignalStatus::Ok || stopped.reading.value.has_value() ||
      stopped.reading.availability != vehicle_core::Availability::Unavailable)
    return 20;

  // Restart clears old-run values; before traffic resumes a valid read returns
  // NoData. A later acquisition fault retains the restarted run's last value.
  clock.set(malformed_time + 1);
  if (!service.start().ok())
    return 21;
  if (!wait_for(
          [&service] { return service.diagnostics().lifecycle == mazda::LifecycleState::Running; }))
    return 22;
  const auto restarted_empty = provider.read(mazda::internal::signals::kEngineRpm);
  if (restarted_empty.status != vehicle_signals::SignalStatus::Ok ||
      restarted_empty.reading.value.has_value() ||
      restarted_empty.reading.availability != vehicle_core::Availability::NoData)
    return 23;

  constexpr vehicle_core::MonotonicTimestamp restarted_time = malformed_time + 2;
  clock.set(restarted_time);
  if (!inject_and_wait(service, source,
                       frame(mazda::candidate::kEngineDataId, restarted_time,
                             {0x01, 0x00, 0x00, 0x64, 0, 0, 0, 0}),
                       1))
    return 24;
  const auto restarted_rpm = provider.read(mazda::internal::signals::kEngineRpm);
  if (!restarted_rpm.reading.value.has_value() ||
      restarted_rpm.reading.value->as_number() != std::optional<double>{64.0})
    return 25;

  source.fail();
  if (!wait_for([&service] {
        return service.diagnostics().transport == vehicle_core::TransportHealth::Faulted;
      }))
    return 26;
  const auto faulted = provider.read(mazda::internal::signals::kEngineRpm);
  if (faulted.status != vehicle_signals::SignalStatus::Ok ||
      faulted.reading.value != restarted_rpm.reading.value ||
      faulted.reading.availability != vehicle_core::Availability::Unavailable)
    return 27;

  (void)service.stop();
  return 0;
}
