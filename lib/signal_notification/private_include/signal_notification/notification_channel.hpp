#pragma once

// Provider-private latest-state notification channel. Only the Mazda and
// generated-profile signal providers (and their tests) may include this
// header; public headers expose only signal_notification/notification.hpp.
//
// Provenance: ported behaviour-for-behaviour from
// components/vehicle_core/include/vehicle_core/notification_channel.hpp of
// Yuke-hd/esp32-vehicle-can-core release 0.2.1
// (b3d87ac094576f7a787114a1bd24db5cf427a4f7), Apache-2.0, same owner. Core
// 0.3.0 removed the channel. Changes from the source: the namespace is
// signal_notification, the contract comes from
// signal_notification/notification.hpp, and vehicle_core value types are
// qualified explicitly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "signal_notification/notification.hpp"
#include "vehicle_core/telemetry_contracts.hpp"

namespace signal_notification {

// Notification channels are deliberately fixed-capacity.  A channel identity
// is supplied by its owner as a compile-time value, so a handle cannot be
// reused with a different named channel.
inline constexpr std::size_t kNotificationSubscribersPerChannel = 2;

template <typename T, std::uint16_t ChannelId> class NotificationChannel;

class NotificationHandle final {
public:
  constexpr NotificationHandle() noexcept = default;

  friend constexpr bool operator==(NotificationHandle left, NotificationHandle right) noexcept {
    return left.channel_ == right.channel_ && left.slot_ == right.slot_ &&
           left.generation_ == right.generation_;
  }
  friend constexpr bool operator!=(NotificationHandle left, NotificationHandle right) noexcept {
    return !(left == right);
  }

private:
  template <typename T, std::uint16_t ChannelId> friend class NotificationChannel;

  constexpr NotificationHandle(std::uint16_t channel, std::uint8_t slot,
                               std::uint16_t generation) noexcept
      : channel_(channel), slot_(slot), generation_(generation) {}

  // These fields are value-only for easy storage by an owner.  They are not a
  // public signal mask or a registry index; channel owners choose the identity
  // at compile time and the slot/generation are validated by the channel.
  std::uint16_t channel_{0xffffU};
  std::uint8_t slot_{0xffU};
  std::uint16_t generation_{0};
};

enum class NotificationStatus : std::uint8_t {
  Ok,
  AlreadyRunning,
  NotRunning,
  InvalidState,
  InvalidCallback,
  InvalidSubscription,
  CapacityExceeded,
  NoPending,
};

template <typename T> struct NotificationSubscriptionResult final {
  NotificationStatus status{NotificationStatus::InvalidState};
  std::optional<NotificationHandle> value{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return status == NotificationStatus::Ok && value.has_value();
  }
  [[nodiscard]] constexpr bool ok() const noexcept {
    return status == NotificationStatus::Ok && value.has_value();
  }
};

using NotificationStatusResult = NotificationStatus;

// A portable latest-state channel.  It owns no task and makes no assumptions
// about the source of a Reading.  The channel owner publishes from its service
// context and calls dispatch_one() from its notification context.
//
// ChannelId is a named-channel identity selected by the owner.  It is part of
// the type and is copied into handles only to reject accidental cross-channel
// unsubscribe calls.  There are exactly two fixed subscriber slots and one
// pending notice per slot; no allocation or ordered event history is used.
template <typename T, std::uint16_t ChannelId> class NotificationChannel final {
public:
  using Value = T;
  using CallbackType = Callback<T>;
  using SubscriptionResult = NotificationSubscriptionResult<T>;

  NotificationChannel() noexcept = default;
  ~NotificationChannel() = default;

  NotificationChannel(const NotificationChannel &) = delete;
  NotificationChannel &operator=(const NotificationChannel &) = delete;
  NotificationChannel(NotificationChannel &&) = delete;
  NotificationChannel &operator=(NotificationChannel &&) = delete;

  [[nodiscard]] static constexpr std::uint16_t channel_id() noexcept { return ChannelId; }

  // Registration is intentionally stopped-only.  A successful registration
  // is retained over stop/start; the next start creates a fresh initial notice.
  [[nodiscard]] SubscriptionResult subscribe(CallbackType callback, void *context) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_active_ || running_) {
      return {NotificationStatus::InvalidState, std::nullopt};
    }
    if (callback == nullptr) {
      return {NotificationStatus::InvalidCallback, std::nullopt};
    }

    for (std::size_t index = 0; index < subscribers_.size(); ++index) {
      Subscriber &subscriber = subscribers_[index];
      if (subscriber.active) {
        continue;
      }

      subscriber.active = true;
      subscriber.callback = callback;
      subscriber.context = context;
      subscriber.pending = false;
      subscriber.pending_initial_seed = false;
      subscriber.pending_notice = Notification<T>{};
      subscriber.registration_order = next_registration_order_++;
      if (next_registration_order_ == 0) {
        next_registration_order_ = 1;
      }

      ++subscriber.generation;
      if (subscriber.generation == 0) {
        subscriber.generation = 1;
      }
      return {
          NotificationStatus::Ok,
          NotificationHandle{ChannelId, static_cast<std::uint8_t>(index), subscriber.generation}};
    }
    return {NotificationStatus::CapacityExceeded, std::nullopt};
  }

  // The shorter name is useful to adapters while keeping the operation
  // explicitly typed and bounded.
  [[nodiscard]] SubscriptionResult register_callback(CallbackType callback,
                                                     void *context) noexcept {
    return subscribe(callback, context);
  }

  [[nodiscard]] NotificationStatus unsubscribe(NotificationHandle handle) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_active_ || running_) {
      return NotificationStatus::InvalidState;
    }
    Subscriber *subscriber = find_subscriber(handle);
    if (subscriber == nullptr) {
      return NotificationStatus::InvalidSubscription;
    }
    subscriber->active = false;
    subscriber->callback = nullptr;
    subscriber->context = nullptr;
    subscriber->pending = false;
    subscriber->pending_initial_seed = false;
    subscriber->pending_notice = Notification<T>{};
    subscriber->registration_order = 0;
    return NotificationStatus::Ok;
  }

  // Start resets run-local state, including the previous availability period
  // and all pending transition flags.  Existing registrations survive.
  [[nodiscard]] NotificationStatus start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_active_ || running_) {
      return NotificationStatus::AlreadyRunning;
    }

    running_ = true;
    dispatching_ = false;
    dispatch_cursor_order_ = 0;
    current_ = vehicle_core::Reading<T>{};
    has_available_period_ = false;
    for (Subscriber &subscriber : subscribers_) {
      subscriber.pending = false;
      subscriber.pending_notice = Notification<T>{};
      subscriber.pending_notice.current = current_;
      subscriber.pending_notice.initial = true;
      subscriber.pending_initial_seed = true;
      if (subscriber.active) {
        subscriber.pending = true;
      }
    }
    return NotificationStatus::Ok;
  }

  // Stop never invokes callbacks.  Pending notices are discarded and are
  // recreated as initial notices by the next start.  A callback in progress
  // is left to its owner; rejecting stop preserves the quiescence boundary.
  [[nodiscard]] NotificationStatus stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_active_ || dispatching_) {
      return NotificationStatus::InvalidState;
    }
    if (!running_) {
      return NotificationStatus::NotRunning;
    }
    running_ = false;
    has_available_period_ = false;
    for (Subscriber &subscriber : subscribers_) {
      subscriber.pending = false;
      subscriber.pending_initial_seed = false;
      subscriber.pending_notice = Notification<T>{};
    }
    return NotificationStatus::Ok;
  }

  [[nodiscard]] bool running() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  [[nodiscard]] vehicle_core::Reading<T> current() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
  }

  // Publish only when the semantic Reading changed.  Timestamp/clock data is
  // intentionally absent from Reading, so equal refreshed observations are
  // suppressed.  Availability transitions use the shared canonical policy:
  // Fresh and FreshnessUnverified are available; all other states are not.
  [[nodiscard]] NotificationStatus publish(const vehicle_core::Reading<T> &reading) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return NotificationStatus::NotRunning;
    }
    if (same_reading(current_, reading)) {
      return NotificationStatus::Ok;
    }

    const bool old_available = available(current_);
    const bool new_available = available(reading);
    const bool became_unavailable = old_available && !new_available;
    const bool recovered = !old_available && new_available && has_available_period_;

    current_ = reading;
    if (new_available) {
      has_available_period_ = true;
    }

    for (Subscriber &subscriber : subscribers_) {
      if (!subscriber.active) {
        continue;
      }
      Notification<T> update{};
      update.current = reading;
      update.became_unavailable = became_unavailable;
      update.recovered = recovered;
      merge_pending(subscriber, update);
    }
    return NotificationStatus::Ok;
  }

  // Detach a notice under the short state lock, then invoke the non-owning
  // callback with the lock released.  A callback's publish() therefore builds
  // the next pending notice instead of being erased by this delivery.
  [[nodiscard]] NotificationStatus dispatch_one() noexcept {
    CallbackType callback = nullptr;
    void *context = nullptr;
    Notification<T> notice{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return NotificationStatus::NotRunning;
      }
      if (callback_active_ || dispatching_) {
        return NotificationStatus::InvalidState;
      }

      Subscriber *subscriber = next_pending_subscriber();
      if (subscriber == nullptr) {
        return NotificationStatus::NoPending;
      }
      callback = subscriber->callback;
      context = subscriber->context;
      notice = subscriber->pending_notice;
      dispatch_cursor_order_ = subscriber->registration_order;
      subscriber->pending = false;
      subscriber->pending_initial_seed = false;
      dispatching_ = true;
      callback_active_ = true;
    }

    // The callback ABI is noexcept by contract.  Keep this invocation outside
    // every publication/state lock even when the callback is user supplied.
    callback(context, notice);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_active_ = false;
      dispatching_ = false;
    }
    return NotificationStatus::Ok;
  }

  // A bounded convenience pass.  It intentionally does not promise to drain
  // an unbounded producer or a callback that publishes on every invocation.
  [[nodiscard]] std::size_t
  dispatch_pending(std::size_t max_callbacks = kNotificationSubscribersPerChannel) noexcept {
    std::size_t delivered = 0;
    while (delivered < max_callbacks && dispatch_one() == NotificationStatus::Ok) {
      ++delivered;
    }
    return delivered;
  }

private:
  struct Subscriber final {
    CallbackType callback{nullptr};
    void *context{nullptr};
    std::uint16_t generation{0};
    std::uint64_t registration_order{0};
    bool active{false};
    bool pending{false};
    bool pending_initial_seed{false};
    Notification<T> pending_notice{};
  };

  [[nodiscard]] static constexpr bool available(const vehicle_core::Reading<T> &reading) noexcept {
    return reading.value.has_value() &&
           (reading.availability == vehicle_core::Availability::Fresh ||
            reading.availability == vehicle_core::Availability::FreshnessUnverified);
  }

  [[nodiscard]] static constexpr bool same_reading(const vehicle_core::Reading<T> &left,
                                                   const vehicle_core::Reading<T> &right) noexcept {
    return left.value == right.value && left.availability == right.availability &&
           left.validation == right.validation;
  }

  [[nodiscard]] Subscriber *find_subscriber(NotificationHandle handle) noexcept {
    if (handle.channel_ != ChannelId || handle.slot_ >= subscribers_.size()) {
      return nullptr;
    }
    Subscriber &subscriber = subscribers_[handle.slot_];
    if (!subscriber.active || subscriber.generation != handle.generation_) {
      return nullptr;
    }
    return &subscriber;
  }

  void merge_pending(Subscriber &subscriber, const Notification<T> &update) noexcept {
    if (!subscriber.pending) {
      subscriber.pending_notice = update;
      subscriber.pending = true;
      subscriber.pending_initial_seed = false;
      return;
    }

    // Initial state is a seed rather than a delivered transition.  The first
    // state observed before its callback runs updates the initial notice
    // without reporting a spurious coalesced transition.
    if (!subscriber.pending_initial_seed) {
      subscriber.pending_notice.coalesced = true;
    }
    subscriber.pending_notice.current = update.current;
    subscriber.pending_notice.became_unavailable =
        subscriber.pending_notice.became_unavailable || update.became_unavailable;
    subscriber.pending_notice.recovered = subscriber.pending_notice.recovered || update.recovered;
    subscriber.pending_initial_seed = false;
  }

  [[nodiscard]] Subscriber *next_pending_subscriber() noexcept {
    Subscriber *selected = nullptr;
    for (Subscriber &subscriber : subscribers_) {
      if (!subscriber.active || !subscriber.pending) {
        continue;
      }
      if (subscriber.registration_order <= dispatch_cursor_order_) {
        continue;
      }
      if (selected == nullptr || subscriber.registration_order < selected->registration_order) {
        selected = &subscriber;
      }
    }

    if (selected != nullptr) {
      return selected;
    }

    // No pending subscriber is newer than the cursor.  Wrap to the oldest
    // pending registration so a callback that keeps publishing cannot starve
    // an earlier subscriber indefinitely.
    for (Subscriber &subscriber : subscribers_) {
      if (!subscriber.active || !subscriber.pending) {
        continue;
      }
      if (selected == nullptr || subscriber.registration_order < selected->registration_order) {
        selected = &subscriber;
      }
    }
    return selected;
  }

  mutable std::mutex mutex_;
  std::array<Subscriber, kNotificationSubscribersPerChannel> subscribers_{};
  vehicle_core::Reading<T> current_{};
  std::uint64_t next_registration_order_{1};
  std::uint64_t dispatch_cursor_order_{0};
  bool has_available_period_{false};
  bool running_{false};
  bool dispatching_{false};
  bool callback_active_{false};
};

} // namespace signal_notification
