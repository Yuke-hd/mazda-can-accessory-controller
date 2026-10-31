#include "notifications.hpp"

#include <cstddef>
#include <cstdint>

#include "provider_access.hpp"
#include "state.hpp"
#include "vehicle_profile_runtime/provider.hpp"

namespace vehicle_profile_runtime {
namespace {

using vehicle_core::Availability;
using vehicle_core::Notification;
using vehicle_core::NotificationStatus;
using vehicle_core::Reading;
using vehicle_signals::SignalCallback;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalId;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalSubscription;
using vehicle_signals::SignalValue;

constexpr std::uint64_t kChannelMask = 0xffU;
constexpr std::uint64_t kSlotMask = 0xffU;
constexpr std::uint64_t kGenerationMask = 0xffffU;
constexpr unsigned kSlotShift = 8U;
constexpr unsigned kGenerationShift = 16U;
constexpr std::uint64_t kUsedBits =
    kChannelMask | (kSlotMask << kSlotShift) | (kGenerationMask << kGenerationShift);

struct DecodedSubscription final {
  std::size_t channel_index{0};
  std::size_t slot{0};
  std::uint16_t generation{0};
};

[[nodiscard]] constexpr std::uint16_t next_generation(const std::uint16_t generation) noexcept {
  const std::uint16_t next = static_cast<std::uint16_t>(generation + 1U);
  return next == 0U ? 1U : next;
}

[[nodiscard]] constexpr SignalSubscription
encode_subscription(const std::size_t channel_index, const std::size_t slot,
                    const std::uint16_t generation) noexcept {
  const std::uint64_t encoded_channel = static_cast<std::uint64_t>(channel_index + 1U);
  return SignalSubscription::from_provider_bits(
      encoded_channel | (static_cast<std::uint64_t>(slot) << kSlotShift) |
      (static_cast<std::uint64_t>(generation) << kGenerationShift));
}

[[nodiscard]] bool decode_subscription(const SignalSubscription subscription,
                                       const NotificationState &notifications,
                                       DecodedSubscription &decoded) noexcept {
  const std::uint64_t bits = subscription.provider_bits();
  if (bits == 0U || (bits & ~kUsedBits) != 0U) {
    return false;
  }

  const std::uint64_t encoded_channel = bits & kChannelMask;
  const std::uint64_t slot = (bits >> kSlotShift) & kSlotMask;
  const std::uint64_t generation = (bits >> kGenerationShift) & kGenerationMask;
  if (encoded_channel == 0U || encoded_channel > notifications.channel_count ||
      slot >= kNotifySubscribersPerBinding || generation == 0U) {
    return false;
  }

  decoded.channel_index = static_cast<std::size_t>(encoded_channel - 1U);
  decoded.slot = static_cast<std::size_t>(slot);
  decoded.generation = static_cast<std::uint16_t>(generation);
  return true;
}

[[nodiscard]] vehicle_signals::SignalStatus
map_notification_status(const NotificationStatus status) noexcept {
  switch (status) {
  case NotificationStatus::Ok:
    return SignalStatus::InvalidState;
  case NotificationStatus::CapacityExceeded:
    return SignalStatus::CapacityExceeded;
  case NotificationStatus::InvalidSubscription:
    return SignalStatus::InvalidSubscription;
  case NotificationStatus::InvalidCallback:
    return SignalStatus::InvalidArgument;
  case NotificationStatus::InvalidState:
  case NotificationStatus::AlreadyRunning:
  case NotificationStatus::NotRunning:
    return SignalStatus::InvalidState;
  case NotificationStatus::NoPending:
    return SignalStatus::InvalidState;
  }
  return SignalStatus::Faulted;
}

[[nodiscard]] Availability
to_core_availability(const vehicle_signals::Availability availability) noexcept {
  switch (availability) {
  case vehicle_signals::Availability::NoData:
    return Availability::NoData;
  case vehicle_signals::Availability::Fresh:
    return Availability::Fresh;
  case vehicle_signals::Availability::Stale:
    return Availability::Stale;
  case vehicle_signals::Availability::FreshnessUnverified:
    return Availability::FreshnessUnverified;
  case vehicle_signals::Availability::Unavailable:
    return Availability::Unavailable;
  }
  return Availability::Unavailable;
}

[[nodiscard]] vehicle_signals::Availability
to_signal_availability(const Availability availability) noexcept {
  switch (availability) {
  case Availability::NoData:
    return vehicle_signals::Availability::NoData;
  case Availability::Fresh:
    return vehicle_signals::Availability::Fresh;
  case Availability::Stale:
    return vehicle_signals::Availability::Stale;
  case Availability::FreshnessUnverified:
    return vehicle_signals::Availability::FreshnessUnverified;
  case Availability::Unavailable:
    return vehicle_signals::Availability::Unavailable;
  }
  return vehicle_signals::Availability::Unavailable;
}

[[nodiscard]] vehicle_signals::ValidationStatus
to_signal_validation(const vehicle_core::ValidationStatus validation) noexcept {
  switch (validation) {
  case vehicle_core::ValidationStatus::Reference:
    return vehicle_signals::ValidationStatus::Reference;
  case vehicle_core::ValidationStatus::Observed:
    return vehicle_signals::ValidationStatus::Observed;
  case vehicle_core::ValidationStatus::Confirmed:
    return vehicle_signals::ValidationStatus::Confirmed;
  }
  return vehicle_signals::ValidationStatus::Reference;
}

[[nodiscard]] vehicle_core::ValidationStatus
to_core_validation(const vehicle_signals::ValidationStatus validation) noexcept {
  switch (validation) {
  case vehicle_signals::ValidationStatus::Reference:
    return vehicle_core::ValidationStatus::Reference;
  case vehicle_signals::ValidationStatus::Observed:
    return vehicle_core::ValidationStatus::Observed;
  case vehicle_signals::ValidationStatus::Confirmed:
    return vehicle_core::ValidationStatus::Confirmed;
  }
  return vehicle_core::ValidationStatus::Reference;
}

[[nodiscard]] Reading<SignalValue>
to_core_reading(const vehicle_signals::SignalReading &reading) noexcept {
  Reading<SignalValue> result{};
  result.value = reading.value;
  result.availability = to_core_availability(reading.availability);
  result.validation = to_core_validation(reading.validation);
  return result;
}

[[nodiscard]] bool available(const Reading<SignalValue> &reading) noexcept {
  return reading.value.has_value() && (reading.availability == Availability::Fresh ||
                                       reading.availability == Availability::FreshnessUnverified);
}

void notification_trampoline(void *context, const Notification<SignalValue> &notice) noexcept {
  auto &registration = *static_cast<NotificationRegistration *>(context);
  if (!registration.active || registration.callback == nullptr ||
      registration.callback_active == nullptr) {
    return;
  }

  registration.callback_active->store(true, std::memory_order_release);
  SignalNotification copied{};
  copied.id = registration.id;
  copied.current.value = notice.current.value;
  copied.current.availability = to_signal_availability(notice.current.availability);
  copied.current.validation = to_signal_validation(notice.current.validation);
  copied.initial = notice.initial;
  copied.became_unavailable = notice.became_unavailable;
  copied.recovered = notice.recovered;
  copied.coalesced = notice.coalesced;
  const SignalCallback callback = registration.callback;
  void *const user_context = registration.context;
  callback(user_context, copied);
  registration.callback_active->store(false, std::memory_order_release);
}

[[nodiscard]] std::size_t find_channel_index(const NotificationState &notifications,
                                             const SignalId id) noexcept {
  for (std::size_t index = 0; index < notifications.channel_count; ++index) {
    if (notifications.channels[index].id == id) {
      return index;
    }
  }
  return notifications.channel_count;
}

template <std::size_t Size>
[[nodiscard]] bool contains(const std::array<SignalId, Size> &values, const std::size_t count,
                            const SignalId id) noexcept {
  const std::size_t bounded_count = count < values.size() ? count : values.size();
  for (std::size_t index = 0; index < bounded_count; ++index) {
    if (values[index] == id) {
      return true;
    }
  }
  return false;
}

void clear_registration(NotificationRegistration &registration) noexcept {
  registration.callback = nullptr;
  registration.context = nullptr;
  registration.callback_active = nullptr;
  registration.channel_handle = {};
  registration.active = false;
}

} // namespace

void initialize_notification_channels(ProfileState &state) noexcept {
  NotificationState &notifications = state.notifications;
  notifications.channel_count = 0;
  for (std::size_t index = 0; index < state.tables.catalog.size(); ++index) {
    const vehicle_signals::SignalMetadata *const metadata = state.tables.catalog.at(index);
    if (!metadata->capabilities.has(SignalCapability::Notify)) {
      continue;
    }

    NotificationChannelBinding &binding = notifications.channels[notifications.channel_count];
    binding.id = metadata->id;
    for (NotificationRegistration &registration : binding.registrations) {
      registration.id = metadata->id;
      registration.callback_active = &notifications.callback_active;
    }
    ++notifications.channel_count;
  }
}

vehicle_signals::SignalResult<SignalSubscription>
ProfileProvider::subscribe(const SignalId id, const SignalCallback callback,
                           void *const context) noexcept {
  using Result = vehicle_signals::SignalResult<SignalSubscription>;
  ProfileState &provider_state = ProviderAccess::state(*this);
  const vehicle_signals::SignalMetadata *const metadata = provider_state.tables.catalog.find(id);
  if (metadata == nullptr) {
    return Result::failure(SignalStatus::InvalidSignal);
  }
  if (!metadata->capabilities.has(SignalCapability::Notify)) {
    return Result::failure(SignalStatus::UnsupportedCapability);
  }
  if (!provider_state.valid_tables) {
    return Result::failure(SignalStatus::Faulted);
  }
  NotificationState &notifications = provider_state.notifications;
  if (notifications.running.load(std::memory_order_acquire) ||
      notifications.callback_active.load(std::memory_order_acquire)) {
    return Result::failure(SignalStatus::InvalidState);
  }
  if (callback == nullptr) {
    return Result::failure(SignalStatus::InvalidArgument);
  }
  const auto owner_check = provider_state.synchronization.claim_or_validate_lifecycle_owner;
  if (owner_check == nullptr ||
      !owner_check(provider_state.synchronization.lifecycle_owner_context)) {
    return Result::failure(SignalStatus::InvalidState);
  }

  const std::size_t channel_index = find_channel_index(notifications, id);
  if (channel_index >= notifications.channel_count) {
    return Result::failure(SignalStatus::Faulted);
  }
  NotificationChannelBinding &binding = notifications.channels[channel_index];
  for (std::size_t slot = 0; slot < binding.registrations.size(); ++slot) {
    NotificationRegistration &registration = binding.registrations[slot];
    if (registration.active) {
      continue;
    }

    registration.callback = callback;
    registration.context = context;
    registration.callback_active = &notifications.callback_active;
    const auto channel_result = binding.channel.subscribe(&notification_trampoline, &registration);
    if (!channel_result.ok()) {
      clear_registration(registration);
      return Result::failure(map_notification_status(channel_result.status));
    }

    registration.generation = next_generation(registration.generation);
    registration.channel_handle = *channel_result.value;
    registration.active = true;
    return Result::success(encode_subscription(channel_index, slot, registration.generation));
  }
  return Result::failure(SignalStatus::CapacityExceeded);
}

vehicle_signals::SignalStatusResult
ProfileProvider::unsubscribe(const SignalSubscription subscription) noexcept {
  using Result = vehicle_signals::SignalStatusResult;
  ProfileState &provider_state = ProviderAccess::state(*this);
  if (!provider_state.valid_tables) {
    return Result::failure(SignalStatus::Faulted);
  }
  NotificationState &notifications = provider_state.notifications;
  if (notifications.running.load(std::memory_order_acquire) ||
      notifications.callback_active.load(std::memory_order_acquire)) {
    return Result::failure(SignalStatus::InvalidState);
  }

  const auto owner_check = provider_state.synchronization.claim_or_validate_lifecycle_owner;
  if (owner_check == nullptr ||
      !owner_check(provider_state.synchronization.lifecycle_owner_context)) {
    return Result::failure(SignalStatus::InvalidState);
  }

  DecodedSubscription decoded{};
  if (!decode_subscription(subscription, notifications, decoded)) {
    return Result::failure(SignalStatus::InvalidSubscription);
  }
  NotificationChannelBinding &binding = notifications.channels[decoded.channel_index];
  NotificationRegistration &registration = binding.registrations[decoded.slot];
  if (!registration.active || registration.generation != decoded.generation) {
    return Result::failure(SignalStatus::InvalidSubscription);
  }

  const NotificationStatus status = binding.channel.unsubscribe(registration.channel_handle);
  if (status != NotificationStatus::Ok) {
    return Result::failure(map_notification_status(status));
  }
  clear_registration(registration);
  return Result::success();
}

void ProviderAccess::set_running(ProfileProvider &provider, const bool running) noexcept {
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables) {
    return;
  }
  NotificationState &notifications = provider_state.notifications;
  if (notifications.callback_active.load(std::memory_order_acquire)) {
    return;
  }

  if (running) {
    if (notifications.running.load(std::memory_order_acquire)) {
      return;
    }
    std::size_t started_count = 0;
    for (; started_count < notifications.channel_count; ++started_count) {
      if (notifications.channels[started_count].channel.start() != NotificationStatus::Ok) {
        for (std::size_t rollback = 0; rollback < started_count; ++rollback) {
          (void)notifications.channels[rollback].channel.stop();
        }
        return;
      }
    }
    notifications.running.store(true, std::memory_order_release);
    for (std::size_t index = 0; index < notifications.channel_count; ++index) {
      NotificationChannelBinding &binding = notifications.channels[index];
      const auto initial = provider.read(binding.id);
      if (initial.ok()) {
        (void)binding.channel.publish(to_core_reading(*initial.value));
      }
    }
    return;
  }

  if (!notifications.running.load(std::memory_order_acquire)) {
    return;
  }
  bool stopped = true;
  for (std::size_t index = 0; index < notifications.channel_count; ++index) {
    if (notifications.channels[index].channel.stop() != NotificationStatus::Ok) {
      stopped = false;
    }
  }
  if (!stopped) {
    return;
  }

  // A successful provider stop ends every borrowed callback-context lifetime.
  // Detach registrations only after the adapter has quiesced its dispatcher.
  for (std::size_t index = 0; index < notifications.channel_count; ++index) {
    NotificationChannelBinding &binding = notifications.channels[index];
    for (NotificationRegistration &registration : binding.registrations) {
      if (!registration.active) {
        continue;
      }
      (void)binding.channel.unsubscribe(registration.channel_handle);
      clear_registration(registration);
    }
  }
  notifications.running.store(false, std::memory_order_release);
}

bool ProviderAccess::callback_active(const ProfileProvider &provider) noexcept {
  return state(provider).notifications.callback_active.load(std::memory_order_acquire);
}

void ProviderAccess::deliver(ProfileProvider &provider,
                             const ProviderProcessResult &result) noexcept {
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables ||
      !provider_state.notifications.running.load(std::memory_order_acquire)) {
    return;
  }

  NotificationState &notifications = provider_state.notifications;
  for (std::size_t index = 0; index < notifications.channel_count; ++index) {
    NotificationChannelBinding &binding = notifications.channels[index];
    const bool due = contains(result.due, result.due_count, binding.id);
    const bool changed = contains(result.changed, result.changed_count, binding.id);
    const bool recovered = contains(result.recovered, result.recovered_count, binding.id);
    if (!due && !changed && !recovered) {
      continue;
    }

    const auto current = provider.read(binding.id);
    if (!current.ok()) {
      continue;
    }

    if (due && recovered) {
      Reading<SignalValue> unavailable = binding.channel.current();
      if (available(unavailable)) {
        unavailable.availability = Availability::Stale;
        (void)binding.channel.publish(unavailable);
      }
    }
    (void)binding.channel.publish(to_core_reading(*current.value));
  }

  // The service adapter owns one serialized delivery path across channels.
  // Each core channel detaches a copied notice and releases its lock before
  // invoking this provider's trampoline.
  for (std::size_t index = 0; index < notifications.channel_count; ++index) {
    (void)notifications.channels[index].channel.dispatch_pending(kNotifySubscribersPerBinding);
  }
}

} // namespace vehicle_profile_runtime
