#include "action_engine/subscription_set.hpp"

namespace action_engine {

using vehicle_signals::SignalStatus;

SignalStatus SubscriptionSet::subscribe(vehicle_signals::SignalProvider &provider,
                                        vehicle_signals::SignalId signal,
                                        vehicle_signals::SignalCallback callback,
                                        void *context) noexcept {
  if (contains(signal)) {
    return SignalStatus::Ok;
  }
  if (count_ == kCapacity) {
    return SignalStatus::CapacityExceeded;
  }
  const auto result = provider.subscribe(signal, callback, context);
  if (!result.ok()) {
    return result.status;
  }
  registrations_[count_++] = Registration{signal, *result.value};
  return SignalStatus::Ok;
}

SignalStatus SubscriptionSet::unsubscribe_all(vehicle_signals::SignalProvider &provider) noexcept {
  while (count_ != 0) {
    const auto result = provider.unsubscribe(registrations_[count_ - 1].subscription);
    if (!result.ok()) {
      return result.status;
    }
    registrations_[--count_] = Registration{};
  }
  return SignalStatus::Ok;
}

bool SubscriptionSet::contains(vehicle_signals::SignalId signal) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (registrations_[index].signal == signal) {
      return true;
    }
  }
  return false;
}

} // namespace action_engine
