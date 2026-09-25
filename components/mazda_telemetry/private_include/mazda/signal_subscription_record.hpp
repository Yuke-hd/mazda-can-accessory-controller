#pragma once

#include <cstddef>
#include <cstdint>

#include "mazda/notification.hpp"
#include "mazda/signal_catalog.hpp"
#include "mazda/signal_value_conversion.hpp"
#include "vehicle_core/notification_channel.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda::internal {

// Fixed callback trampoline record for one generic subscription. The service
// owns an array of them, so a record lives exactly as long as the typed
// channels that point at it: it is released by a generic unsubscribe or by
// the service destructor after its stop and dispatcher join, the same
// guarantee typed callbacks have. The typed channel stores a pointer to the
// record as its callback context. Records are written only by stopped-only
// lifecycle-owner mutations under the service lifecycle mutex and are read by
// the dispatcher only while registered, so delivery neither allocates nor
// locks. `channel`/`slot`/`generation` are the underlying typed token.
struct SignalSubscriptionRecord final {
  vehicle_signals::SignalId id{};
  vehicle_signals::SignalCallback callback{nullptr};
  void *context{nullptr};
  std::uint16_t channel{0xffffU};
  std::uint8_t slot{0xffU};
  std::uint16_t generation{0};
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
inline constexpr std::size_t kGenericSubscriptionCapacity =
    notify_signal_count() * vehicle_core::kNotificationSubscribersPerChannel;
static_assert(notify_signal_count() == 16);

// Typed channel callback registered for a generic subscription. It converts
// `current` and exactly the four flags and forwards them with the user
// context; the record is unchanged while the channel can dispatch to it.
template <typename T>
void signal_subscription_trampoline(void *record, const Notification<T> &notice) noexcept {
  const auto &subscription = *static_cast<const SignalSubscriptionRecord *>(record);
  subscription.callback(subscription.context, to_signal_notification(subscription.id, notice));
}

} // namespace mazda::internal
