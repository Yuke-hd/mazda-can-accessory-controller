#include "mazda/signal_provider_internal.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace mazda {
namespace {

using vehicle_signals::SignalId;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalValue;

template <typename T> SignalValue generic_value(const T &value) noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return SignalValue::boolean(value);
  } else if constexpr (std::is_arithmetic_v<T>) {
    return SignalValue::number(static_cast<double>(value));
  } else {
    static_assert(std::is_enum_v<T>, "generic signal values support only numbers and enums");
    return SignalValue::enumeration(static_cast<std::int32_t>(value));
  }
}

SignalStatus generic_status(const ResultCode status) noexcept {
  switch (status) {
  case ResultCode::Ok:
    return SignalStatus::Ok;
  case ResultCode::CapacityExceeded:
    return SignalStatus::CapacityExceeded;
  case ResultCode::InvalidSubscription:
    return SignalStatus::InvalidSubscription;
  case ResultCode::InvalidState:
  case ResultCode::InvalidConfiguration:
  case ResultCode::AlreadyRunning:
  case ResultCode::NotRunning:
  case ResultCode::Faulted:
  case ResultCode::Timeout:
    return SignalStatus::InvalidState;
  }
  return SignalStatus::InvalidState;
}

} // namespace

MazdaSignalProvider::Impl::Impl(internal::VehicleTelemetryService &service) noexcept
    : service_(&service) {}

MazdaSignalProvider::Impl::~Impl() noexcept {
  // Registrations survive stop/start cycles, so release every active channel
  // slot before the provider-owned callback records leave scope. The provider
  // contract requires destruction on the lifecycle owner after a successful
  // stop; this operation deliberately does not stop or wait for the service.
  if (service_ == nullptr)
    return;
  for (auto &record : callbacks_) {
    if (record.active)
      (void)service_->unsubscribe(record.typed_subscription);
  }
}

template <typename T>
void MazdaSignalProvider::Impl::callback_trampoline(
    void *context, const vehicle_core::Notification<T> &notification) noexcept {
  auto *record = static_cast<CallbackRecord *>(context);
  if (record == nullptr || !record->active || record->callback == nullptr)
    return;

  SignalNotification generic{};
  if (notification.current.value.has_value())
    generic.current.value = generic_value(*notification.current.value);
  generic.current.availability = notification.current.availability;
  generic.current.validation = notification.current.validation;
  generic.initial = notification.initial;
  generic.became_unavailable = notification.became_unavailable;
  generic.recovered = notification.recovered;
  generic.coalesced = notification.coalesced;
  record->callback(record->context, record->signal, generic);
}

template <typename T, std::uint16_t ChannelId>
bool MazdaSignalProvider::Impl::subscribe_descriptor(
    internal::VehicleTelemetryService &service,
    const internal::NotificationDescriptor<T, ChannelId> &descriptor, const SignalId signal,
    CallbackRecord &record, TypedSubscription &subscription) noexcept {
  if (descriptor.signal_id != signal)
    return false;
  subscription = service.subscribe_notification_descriptor(descriptor, &callback_trampoline<T>,
                                                           static_cast<void *>(&record));
  return true;
}

MazdaSignalProvider::Impl::CallbackRecord *MazdaSignalProvider::Impl::free_record() noexcept {
  for (auto &record : callbacks_) {
    if (!record.active)
      return &record;
  }
  return nullptr;
}

MazdaSignalProvider::Impl::CallbackRecord *MazdaSignalProvider::Impl::find_record(
    const vehicle_signals::SignalSubscriptionToken token) noexcept {
  if (!token.valid() || token.slot >= vehicle_core::kNotificationSubscribersPerChannel)
    return nullptr;
  for (auto &record : callbacks_) {
    if (record.active && record.signal == token.signal &&
        record.typed_subscription.slot == token.slot &&
        record.typed_subscription.generation == token.generation)
      return &record;
  }
  return nullptr;
}

vehicle_signals::SignalSubscriptionResult MazdaSignalProvider::Impl::subscribe(
    const SignalId signal, const vehicle_signals::SignalCallback callback, void *context) noexcept {
  const auto *metadata = internal::signals::catalog().find(signal);
  if (metadata == nullptr)
    return {SignalStatus::InvalidSignal, {}};
  if (!metadata->capabilities.subscribable)
    return {SignalStatus::UnsupportedCapability, {}};
  if (callback == nullptr)
    return {SignalStatus::InvalidCallback, {}};

  CallbackRecord *record = free_record();
  if (record == nullptr)
    return {SignalStatus::CapacityExceeded, {}};

  record->signal = signal;
  record->callback = callback;
  record->context = context;

  TypedSubscription subscription{};
  const bool matched = std::apply(
      [this, signal, record, &subscription](const auto &...descriptor) {
        return (subscribe_descriptor(*service_, descriptor, signal, *record, subscription) || ...);
      },
      internal::VehicleTelemetryService::notification_descriptors());
  if (!matched) {
    *record = CallbackRecord{};
    return {SignalStatus::InvalidState, {}};
  }
  if (!subscription.ok()) {
    *record = CallbackRecord{};
    return {generic_status(subscription.status), {}};
  }
  if (subscription.slot >= vehicle_core::kNotificationSubscribersPerChannel ||
      subscription.generation == 0) {
    *record = CallbackRecord{};
    (void)service_->unsubscribe(subscription);
    return {SignalStatus::InvalidState, {}};
  }

  record->typed_subscription = subscription;
  record->active = true;
  return {SignalStatus::Ok,
          {signal, subscription.slot, static_cast<std::uint32_t>(subscription.generation)}};
}

vehicle_signals::SignalStatus MazdaSignalProvider::Impl::unsubscribe(
    const vehicle_signals::SignalSubscriptionToken token) noexcept {
  CallbackRecord *record = find_record(token);
  if (record == nullptr)
    return SignalStatus::InvalidSubscription;

  const StatusResult result = service_->unsubscribe(record->typed_subscription);
  if (!result.ok())
    return generic_status(result.status);

  *record = CallbackRecord{};
  return SignalStatus::Ok;
}

vehicle_signals::SignalSubscriptionResult
MazdaSignalProvider::subscribe(const vehicle_signals::SignalId signal,
                               const vehicle_signals::SignalCallback callback,
                               void *context) noexcept {
  auto *implementation = reinterpret_cast<Impl *>(implementation_storage_);
  return implementation->subscribe(signal, callback, context);
}

vehicle_signals::SignalStatus
MazdaSignalProvider::unsubscribe(const vehicle_signals::SignalSubscriptionToken token) noexcept {
  auto *implementation = reinterpret_cast<Impl *>(implementation_storage_);
  return implementation->unsubscribe(token);
}

} // namespace mazda
