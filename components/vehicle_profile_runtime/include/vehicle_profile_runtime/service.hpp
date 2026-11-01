#pragma once

#include <cstddef>
#include <cstdint>

#include "vehicle_signals/signal_provider.hpp"
#include "vehicle_telemetry/lifecycle.hpp"

namespace vehicle_core {
class MonotonicClock;
}

namespace vehicle_telemetry {
class AcquisitionSource;
}

namespace vehicle_profile_runtime {

struct ProfileTables;

// The opaque adapter contains the pinned runtime, provider, and fixed adapter
// state. Keep this below the Milestone 7 reference ceilings of 28 KiB on ESP32
// and 32 KiB on host. R6 measures the retained implementation.
#if defined(ESP_PLATFORM)
inline constexpr std::size_t kProfileRuntimeServiceStorageBytes = 26U * 1024U;
#else
inline constexpr std::size_t kProfileRuntimeServiceStorageBytes = 30U * 1024U;
#endif

// Owns the receive-side adapter for one immutable generated profile. The
// profile tables, clock, and acquisition source are borrowed and must outlive
// the service. Callback contexts remain borrowed until unsubscribe succeeds or
// stop succeeds; a timed-out stop retains them for a later stop retry.
class ProfileRuntimeService final {
public:
  ProfileRuntimeService(const ProfileTables &tables, vehicle_core::MonotonicClock &clock,
                        vehicle_telemetry::AcquisitionSource &source) noexcept;
  ~ProfileRuntimeService() noexcept;

  ProfileRuntimeService(const ProfileRuntimeService &) = delete;
  ProfileRuntimeService &operator=(const ProfileRuntimeService &) = delete;
  ProfileRuntimeService(ProfileRuntimeService &&) = delete;
  ProfileRuntimeService &operator=(ProfileRuntimeService &&) = delete;

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept;
  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept;

  // Consumers interact only through the generic, value-only provider port.
  [[nodiscard]] vehicle_signals::SignalProvider &provider() noexcept;
  [[nodiscard]] const vehicle_signals::SignalProvider &provider() const noexcept;

private:
  alignas(std::max_align_t) std::byte storage_[kProfileRuntimeServiceStorageBytes]{};
};

} // namespace vehicle_profile_runtime
