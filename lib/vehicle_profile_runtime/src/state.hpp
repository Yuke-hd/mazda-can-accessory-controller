#pragma once

#include <array>
#include <cstdint>

#include "vehicle_profile_runtime/provider.hpp"
#include "notifications.hpp"

namespace vehicle_profile_runtime {

inline constexpr std::uint8_t kHasWatermark = 1U << 0U;
inline constexpr std::uint8_t kHasValue = 1U << 1U;
inline constexpr std::uint8_t kInvalid = 1U << 2U;
inline constexpr std::uint8_t kDueReported = 1U << 3U;

struct PublicationGuard final {
  explicit PublicationGuard(const PublicationSynchronization &binding) noexcept
      : binding_(&binding) {
    binding_->enter(binding_->context);
  }

  ~PublicationGuard() noexcept { binding_->leave(binding_->context); }

  PublicationGuard(const PublicationGuard &) = delete;
  PublicationGuard &operator=(const PublicationGuard &) = delete;

private:
  const PublicationSynchronization *binding_;
};

struct SignalState final {
  vehicle_signals::SignalValue value{};
  vehicle_core::MonotonicTimestamp sample_timestamp_us{0};
  vehicle_core::MonotonicTimestamp watermark_us{0};
  std::uint8_t flags{0};
};

struct ProfileState final {
  ProfileState(ProfileTables profile_tables, vehicle_core::MonotonicClock &profile_clock,
               PublicationSynchronization publication_synchronization) noexcept
      : tables(profile_tables), clock(&profile_clock),
        synchronization(publication_synchronization) {}

  ProfileTables tables{};
  vehicle_core::MonotonicClock *clock{nullptr};
  PublicationSynchronization synchronization{};
  bool valid_tables{false};
  std::array<SignalState, kMaxRuntimeSignals> signals{};
  std::array<DecodeResult, kMaxRuntimeSignals> decode_results{};
  vehicle_core::MonotonicTimestamp receive_watermark_us{0};
  bool has_receive_watermark{false};
  std::uint32_t generation{0};
  NotificationState notifications{};
};

} // namespace vehicle_profile_runtime
