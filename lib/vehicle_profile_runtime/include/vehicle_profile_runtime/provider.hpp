#pragma once

#include <cstddef>
#include <cstdint>

#include "vehicle_profile_runtime/tables.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace vehicle_profile_runtime {

inline constexpr std::size_t kMaxRuntimeSignals = 64;
inline constexpr std::size_t kMaxRuntimeMessages = 64;
// Includes the provider state and up to 17 fixed two-subscriber Notify channels.
inline constexpr std::size_t kProviderStorageBytes = 9216;

// Non-owning view over one immutable generated profile. The view and every
// table it references must outlive the provider.
struct ProfileTables final {
  vehicle_signals::SignalCatalogView catalog{};
  const SignalFreshnessPolicy *freshness_policies{nullptr};
  std::size_t freshness_policy_count{0};
  const DispatchEntry *dispatch{nullptr};
  std::size_t dispatch_count{0};
  std::size_t max_decode_results{0};
};

// Borrowed synchronization and lifecycle-owner binding. Its contexts and
// callbacks must outlive the provider. Publication callbacks use fixed,
// non-allocating storage safe for concurrent reads and the single processing
// owner. Notify subscriptions require the lifecycle check; the adapter shares
// that owner check with start/stop so the first lifecycle mutation claims one
// stable owner across later stop/start cycles.
struct PublicationSynchronization final {
  using Operation = void (*)(void *context) noexcept;
  using LifecycleOwnerCheck = bool (*)(void *context) noexcept;

  void *context{nullptr};
  Operation enter{nullptr};
  Operation leave{nullptr};
  void *lifecycle_owner_context{nullptr};
  LifecycleOwnerCheck claim_or_validate_lifecycle_owner{nullptr};

  [[nodiscard]] constexpr bool well_formed() const noexcept {
    return enter != nullptr && leave != nullptr &&
           ((lifecycle_owner_context == nullptr) == (claim_or_validate_lifecycle_owner == nullptr));
  }
};

struct ProviderAccess;

// Portable latest-state provider for one compiled profile. CAN processing and
// state-transition entry points are private to the runtime and adapter seams.
class ProfileProvider final : public vehicle_signals::SignalProvider {
public:
  ProfileProvider(ProfileTables tables, vehicle_core::MonotonicClock &clock,
                  PublicationSynchronization synchronization) noexcept;
  ~ProfileProvider() noexcept;

  ProfileProvider(const ProfileProvider &) = delete;
  ProfileProvider &operator=(const ProfileProvider &) = delete;
  ProfileProvider(ProfileProvider &&) = delete;
  ProfileProvider &operator=(ProfileProvider &&) = delete;

  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept override;
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read(vehicle_signals::SignalId id) const noexcept override;

  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(vehicle_signals::SignalId id, vehicle_signals::SignalCallback callback,
            void *context) noexcept override;
  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription subscription) noexcept override;

private:
  friend struct ProviderAccess;

  alignas(std::max_align_t) std::byte storage_[kProviderStorageBytes]{};
};

} // namespace vehicle_profile_runtime
