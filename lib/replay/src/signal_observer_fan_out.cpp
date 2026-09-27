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
  if (!subscribe_notified_signals()) {
    (void)detach();
    return false;
  }
  attached_ = true;
  return true;
}

void SignalObserverFanOut::open() noexcept {
  if (!attached_ || opened_)
    return;
  opened_ = true;
  if (observers_.empty())
    return;
  const auto catalog = provider_->catalog();
  for (auto *observer : observers_)
    observer->on_catalog(catalog);
  for (const auto &held : held_)
    publish(held.time_us, *held.signal, held.reading);
  held_.clear();
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
    publish(clock_->now(), signal, *reading.value);
  }
  return true;
}

bool SignalObserverFanOut::detach() noexcept {
  bool released = true;
  for (const auto subscription : subscriptions_)
    released = provider_->unsubscribe(subscription).ok() && released;
  subscriptions_.clear();
  held_.clear();
  attached_ = false;
  opened_ = false;
  return released;
}

void SignalObserverFanOut::forward(
    void *context, const vehicle_signals::SignalNotification &notification) noexcept {
  auto *fan_out = static_cast<SignalObserverFanOut *>(context);
  const auto *signal = fan_out->provider_->catalog().find(notification.id);
  if (signal != nullptr)
    fan_out->receive(*signal, notification.current);
}

void SignalObserverFanOut::receive(const SignalMetadata &signal,
                                   const vehicle_signals::SignalReading &reading) noexcept {
  const auto now = clock_->now();
  if (opened_) {
    publish(now, signal, reading);
    return;
  }
  try {
    held_.push_back({now, &signal, reading});
  } catch (...) {
    // Out of memory while starting: the reading is dropped rather than
    // thrown across the provider's callback.
  }
}

void SignalObserverFanOut::publish(vehicle_core::MonotonicTimestamp now_us,
                                   const SignalMetadata &signal,
                                   const vehicle_signals::SignalReading &reading) const noexcept {
  for (auto *observer : observers_)
    observer->on_reading(now_us, signal, reading);
}

} // namespace replay
