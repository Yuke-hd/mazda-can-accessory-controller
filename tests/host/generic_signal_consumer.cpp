#include "generic_signal_consumer.hpp"

namespace generic_signal_consumer {

using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;
using vehicle_signals::SignalStatus;

std::string_view choice_key(const SignalMetadata &signal, const SignalReading &reading) noexcept {
  if (!reading.value.has_value()) {
    return {};
  }
  const auto raw = reading.value->as_enumeration();
  if (!raw.has_value()) {
    return {};
  }
  const auto *choice = signal.find_choice(*raw);
  return choice == nullptr ? std::string_view{} : choice->key;
}

GenericSignalConsumer::GenericSignalConsumer(mazda::MazdaSignalProvider &provider) noexcept
    : provider_(&provider) {}

SignalStatus GenericSignalConsumer::attach() noexcept {
  const auto catalog = provider_->catalog();
  const SignalMetadata *engine_rpm = catalog.find(kEngineRpmKey);
  const SignalMetadata *turn_state = catalog.find(kTurnStateKey);
  if (engine_rpm == nullptr || turn_state == nullptr) {
    return SignalStatus::InvalidSignal;
  }
  if (!engine_rpm->capabilities.has(SignalCapability::Read) ||
      !turn_state->capabilities.has(SignalCapability::Notify)) {
    return SignalStatus::UnsupportedCapability;
  }
  const auto subscription = provider_->subscribe(turn_state->id, &on_turn_state, this);
  if (!subscription.ok()) {
    return subscription.status;
  }
  engine_rpm_ = engine_rpm;
  turn_state_ = turn_state;
  turn_subscription_ = *subscription.value;
  return SignalStatus::Ok;
}

SignalStatus GenericSignalConsumer::detach() noexcept {
  const auto result = provider_->unsubscribe(turn_subscription_);
  if (result.ok()) {
    turn_subscription_ = {};
  }
  return result.status;
}

SignalResult<SignalReading> GenericSignalConsumer::read_engine_rpm() const noexcept {
  if (engine_rpm_ == nullptr) {
    return SignalResult<SignalReading>::failure(SignalStatus::InvalidSignal);
  }
  return provider_->read(engine_rpm_->id);
}

std::optional<SignalNotification> GenericSignalConsumer::latest_turn_notice() const {
  std::lock_guard<std::mutex> lock{mutex_};
  if (turn_notices_ == 0) {
    return std::nullopt;
  }
  return latest_turn_;
}

std::size_t GenericSignalConsumer::turn_notice_count() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return turn_notices_;
}

std::string_view GenericSignalConsumer::turn_choice(const SignalReading &reading) const noexcept {
  return turn_state_ == nullptr ? std::string_view{} : choice_key(*turn_state_, reading);
}

void GenericSignalConsumer::on_turn_state(void *context,
                                          const SignalNotification &notice) noexcept {
  auto &consumer = *static_cast<GenericSignalConsumer *>(context);
  std::lock_guard<std::mutex> lock{consumer.mutex_};
  consumer.latest_turn_ = notice;
  ++consumer.turn_notices_;
}

} // namespace generic_signal_consumer
