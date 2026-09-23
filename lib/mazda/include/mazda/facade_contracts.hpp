#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "mazda/freshness.hpp"
#include "mazda/notification.hpp"
#include "mazda/reading.hpp"
#include "mazda/types.hpp"
#include "vehicle_core/health.hpp"
#include "vehicle_core/time.hpp"

namespace mazda {

using vehicle_core::Callback;
using vehicle_core::Microseconds;
using vehicle_core::TransportHealth;

inline constexpr std::size_t kNotifySubscribersPerChannel = 2;
inline constexpr Microseconds kDefaultTransportSilenceTimeoutUs = 1'000'000;
inline constexpr Microseconds kDefaultCallbackStopTimeoutUs = 500'000;
inline constexpr std::uint8_t kDefaultMaxFramesPerBatch = 16;
inline constexpr Microseconds kDefaultAvailabilityServiceTargetUs = 10'000;

enum class ResultCode : std::uint8_t {
  Ok,
  InvalidConfiguration,
  InvalidState,
  CapacityExceeded,
  InvalidSubscription,
  AlreadyRunning,
  NotRunning,
  Faulted,
  Timeout,
};

template <typename T> struct Result {
  ResultCode status{ResultCode::InvalidState};
  std::optional<T> value{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return status == ResultCode::Ok && value.has_value();
  }
  [[nodiscard]] constexpr bool ok() const noexcept {
    return status == ResultCode::Ok && value.has_value();
  }
};

template <> struct Result<void> {
  ResultCode status{ResultCode::InvalidState};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return status == ResultCode::Ok;
  }
  [[nodiscard]] constexpr bool ok() const noexcept { return status == ResultCode::Ok; }
};

using StatusResult = Result<void>;

// Opaque slot plus generation prevents a handle from unsubscribing a later
// registration that reused the same fixed subscriber slot.
class Subscription final {
public:
  constexpr Subscription() noexcept = default;
  friend constexpr bool operator==(Subscription left, Subscription right) noexcept {
    return left.channel_ == right.channel_ && left.slot_ == right.slot_ &&
           left.generation_ == right.generation_;
  }
  friend constexpr bool operator!=(Subscription left, Subscription right) noexcept {
    return !(left == right);
  }

private:
  friend class VehicleTelemetry;
  constexpr Subscription(std::uint16_t channel, std::uint8_t slot,
                         std::uint16_t generation) noexcept
      : channel_(channel), slot_(slot), generation_(generation) {}
  std::uint16_t channel_{0xffff};
  std::uint8_t slot_{0xff};
  std::uint16_t generation_{0};
};

struct TelemetryConfig {
  VehicleFreshnessPolicy freshness{};
  Microseconds transport_silence_timeout_us{kDefaultTransportSilenceTimeoutUs};
  Microseconds callback_stop_timeout_us{kDefaultCallbackStopTimeoutUs};
  std::uint8_t max_frames_per_batch{kDefaultMaxFramesPerBatch};
  Microseconds availability_service_target_us{kDefaultAvailabilityServiceTargetUs};
};

// A facade has one lifecycle owner: the host thread or ESP-IDF task that
// performs its first lifecycle mutation (configure, subscription, sink
// binding, start, or stop). That context owns later lifecycle mutations across
// stop/start cycles. Typed callbacks are value-only and must not call
// configure(), start(), stop(), or subscription methods. Their context remains
// borrowed until a successful stop() returns; a timeout or other stop failure
// leaves callbacks potentially active and requires a retry before context
// storage or the facade is released. Polling and diagnostics may be called
// from any context.

enum class LifecycleState : std::uint8_t { Stopped, Running, Stopping, Faulted };

struct AcquisitionMetrics {
  std::uint64_t frames_received{0};
  std::uint64_t frames_processed{0};
  std::uint64_t frames_dropped{0};
  std::uint64_t queue_overflows{0};
  std::uint64_t driver_errors{0};
  std::uint64_t missed_frames{0};
  std::uint64_t controller_resets{0};
  std::uint64_t bus_off_events{0};
};

struct Diagnostics {
  LifecycleState lifecycle{LifecycleState::Stopped};
  TransportHealth transport{TransportHealth::Stopped};
  AcquisitionMetrics acquisition{};
};

} // namespace mazda
