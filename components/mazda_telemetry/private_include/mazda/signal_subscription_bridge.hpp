#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

#include "mazda/signal_catalog.hpp"
#include "mazda/vehicle_telemetry_service.hpp"
#include "vehicle_core/notification_channel.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda::internal {

// Fixed callback trampoline record for one generic subscription. The typed
// notification channel stores a pointer to the record as its callback
// context; the trampoline converts the typed notice and forwards it to the
// user callback with the user context. Records are written only by the
// provider's stopped-only mutations and are read by the dispatcher only while
// they are registered, so delivery neither allocates nor locks.
struct SignalSubscriptionRecord final {
  vehicle_signals::SignalId id{};
  vehicle_signals::SignalCallback callback{nullptr};
  void *context{nullptr};
  SubscriptionToken token{};
  bool active{false};
};

[[nodiscard]] constexpr std::size_t notify_signal_count() noexcept {
  std::size_t count = 0;
  for (const auto &entry : kSignalCatalog) {
    if (entry.capabilities.has(vehicle_signals::SignalCapability::Notify))
      ++count;
  }
  return count;
}

// One record per typed subscriber slot of every Notify catalog signal. Typed
// and generic subscribers share those slots, so this bound is never the
// limiting capacity.
inline constexpr std::size_t kSignalSubscriptionRecordCount =
    notify_signal_count() * vehicle_core::kNotificationSubscribersPerChannel;

struct SignalSubscriptionBridge final {
  // Serializes provider-side record mutation. The service already rejects
  // non-owner, running and callback-originated mutations, but it decides that
  // only after the record is filled; a concurrent rejected caller therefore
  // must not share a free record with the lifecycle owner.
  std::atomic<bool> mutating{false};
  std::array<SignalSubscriptionRecord, kSignalSubscriptionRecordCount> records{};
};

// The provider destructor releases the registrations; the bridge storage
// itself needs no destructor call.
static_assert(std::is_trivially_destructible_v<SignalSubscriptionBridge>);
static_assert(notify_signal_count() == 16);

[[nodiscard]] inline SignalSubscriptionBridge &subscription_bridge(std::byte *storage) noexcept {
  return *std::launder(reinterpret_cast<SignalSubscriptionBridge *>(storage));
}

} // namespace mazda::internal
