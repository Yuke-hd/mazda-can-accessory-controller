#include "signal_observer_fan_out.hpp"

#include <algorithm>
#include <utility>

namespace replay {
namespace {

using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;

[[nodiscard]] bool notified(const SignalMetadata &signal) noexcept {
  return signal.capabilities.has(SignalCapability::Notify);
}

[[nodiscard]] bool polled(const SignalMetadata &signal) noexcept {
  return signal.capabilities.has(SignalCapability::Read) && !notified(signal);
}

} // namespace

SignalObserverFanOut::SignalObserverFanOut(vehicle_signals::SignalProvider &provider,
                                           const vehicle_core::MonotonicClock &clock,
                                           SignalObservers observers) noexcept
    : provider_(&provider), clock_(&clock), observers_(std::move(observers)) {}

bool SignalObserverFanOut::configured() const noexcept {
  return std::none_of(observers_.begin(), observers_.end(),
                      [](const SignalObserver *observer) { return observer == nullptr; });
}

bool SignalObserverFanOut::attach() noexcept {
  if (attached_ || !configured())
    return false;
  if (observers_.empty()) {
    attached_ = true;
    return true;
  }
  const auto catalog = provider_->catalog();
  for (auto *observer : observers_)
    observer->on_catalog(catalog);
  if (!subscribe_notified_signals()) {
    (void)detach();
    return false;
  }
  attached_ = true;
  return true;
}

bool SignalObserverFanOut::subscribe_notified_signals() noexcept {
  try {
    for (const auto &signal : provider_->catalog()) {
      if (!notified(signal))
        continue;
      const auto subscription =
          provider_->subscribe(signal.id, &SignalObserverFanOut::forward, this);
      if (!subscription.ok())
        return false;
      subscriptions_.push_back(*subscription.value);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool SignalObserverFanOut::sample() const noexcept {
  if (observers_.empty())
    return true;
  for (const auto &signal : provider_->catalog()) {
    if (!polled(signal))
      continue;
    const auto reading = provider_->read(signal.id);
    if (!reading.ok())
      return false;
    publish(signal, *reading.value);
  }
  return true;
}

bool SignalObserverFanOut::detach() noexcept {
  bool released = true;
  for (const auto subscription : subscriptions_)
    released = provider_->unsubscribe(subscription).ok() && released;
  subscriptions_.clear();
  attached_ = false;
  return released;
}

void SignalObserverFanOut::forward(
    void *context, const vehicle_signals::SignalNotification &notification) noexcept {
  const auto *fan_out = static_cast<const SignalObserverFanOut *>(context);
  const auto *signal = fan_out->provider_->catalog().find(notification.id);
  if (signal != nullptr)
    fan_out->publish(*signal, notification.current);
}

void SignalObserverFanOut::publish(const SignalMetadata &signal,
                                   const vehicle_signals::SignalReading &reading) const noexcept {
  const auto now = clock_->now();
  for (auto *observer : observers_)
    observer->on_reading(now, signal, reading);
}

} // namespace replay
