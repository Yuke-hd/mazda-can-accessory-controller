#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vehicle_profile_runtime/provider.hpp"

namespace vehicle_profile_runtime {

struct ProfileState;

enum class ProviderProcessStatus : std::uint8_t {
  Ignored,
  Applied,
  Malformed,
  InvalidResults,
  Faulted,
};

// Bounded evidence returned to the notification and acquisition adapters.
// No callbacks run while the provider commits a frame or invalidation.
struct ProviderProcessResult final {
  ProviderProcessStatus status{ProviderProcessStatus::Ignored};
  std::array<vehicle_signals::SignalId, kMaxRuntimeSignals> changed{};
  std::size_t changed_count{0};
  std::array<vehicle_signals::SignalId, kMaxRuntimeSignals> due{};
  std::size_t due_count{0};
  std::array<vehicle_signals::SignalId, kMaxRuntimeSignals> recovered{};
  std::size_t recovered_count{0};
};

struct ProviderAccess final {
  [[nodiscard]] static ProfileState &state(ProfileProvider &provider) noexcept;
  [[nodiscard]] static const ProfileState &state(const ProfileProvider &provider) noexcept;

  // The owner serializes process/service/fault/reset calls. Decoder scratch is
  // reused outside the publication synchronization, and decoders, clocks and
  // callbacks never run while that synchronization is entered.
  [[nodiscard]] static ProviderProcessResult
  process(ProfileProvider &provider, const vehicle_core::RawCanFrame &frame) noexcept;
  [[nodiscard]] static ProviderProcessResult service(ProfileProvider &provider) noexcept;
  [[nodiscard]] static ProviderProcessResult fault(ProfileProvider &provider) noexcept;
  [[nodiscard]] static ProviderProcessResult reset(ProfileProvider &provider) noexcept;
};

} // namespace vehicle_profile_runtime
