#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace test_support {

// Host fake of the generic provider port over a caller-supplied catalog. It
// enforces the port contract a real provider enforces: reads only of
// Read-capable catalog signals (NoData until set_reading()), subscriptions
// only to Notify-capable catalog signals, a bounded two-slot capacity per
// signal (as the Mazda channels have), and stopped-only subscription
// mutation. publish()
// delivers a notice synchronously on the calling thread, which serializes
// delivery as the port requires, to every subscriber of its signal, and only
// while running.
class FakeSignalProvider final : public vehicle_signals::SignalProvider {
public:
  static constexpr std::size_t kSubscribersPerSignal = 2;
  static constexpr std::size_t kMaxSubscriptions = 32;
  static constexpr std::size_t kMaxReadings = 32;

  explicit FakeSignalProvider(vehicle_signals::SignalCatalogView catalog) noexcept
      : catalog_(catalog) {}

  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept override {
    return catalog_;
  }

  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read(vehicle_signals::SignalId id) const noexcept override {
    using Result = vehicle_signals::SignalResult<vehicle_signals::SignalReading>;
    const vehicle_signals::SignalMetadata *signal = catalog_.find(id);
    if (signal == nullptr) {
      return Result::failure(vehicle_signals::SignalStatus::InvalidSignal);
    }
    if (!signal->capabilities.has(vehicle_signals::SignalCapability::Read)) {
      return Result::failure(vehicle_signals::SignalStatus::UnsupportedCapability);
    }
    const ReadSlot *slot = read_slot(id);
    if (slot == nullptr) {
      return Result::success(vehicle_signals::SignalReading{});
    }
    if (slot->failure.has_value()) {
      return Result::failure(*slot->failure);
    }
    return Result::success(slot->reading);
  }

  // Stores the reading read(id) returns and clears any injected failure.
  void set_reading(vehicle_signals::SignalId id,
                   const vehicle_signals::SignalReading &reading) noexcept {
    ReadSlot *slot = writable_read_slot(id);
    if (slot != nullptr) {
      slot->reading = reading;
      slot->failure.reset();
    }
  }
  // Makes read(id) fail with `status` (for example Faulted or Timeout) until
  // the next set_reading(id).
  void fail_reads(vehicle_signals::SignalId id, vehicle_signals::SignalStatus status) noexcept {
    ReadSlot *slot = writable_read_slot(id);
    if (slot != nullptr) {
      slot->failure = status;
    }
  }

  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(vehicle_signals::SignalId id, vehicle_signals::SignalCallback callback,
            void *context) noexcept override {
    using Result = vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>;
    const auto status = subscribe_status(id, callback);
    if (status != vehicle_signals::SignalStatus::Ok) {
      return Result::failure(status);
    }
    Slot *slot = free_slot();
    if (slot == nullptr) {
      return Result::failure(vehicle_signals::SignalStatus::CapacityExceeded);
    }
    *slot = Slot{id, callback, context, ++issued_};
    return Result::success(vehicle_signals::SignalSubscription::from_provider_bits(slot->bits));
  }

  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription subscription) noexcept override {
    using Result = vehicle_signals::SignalStatusResult;
    if (running_) {
      return Result::failure(vehicle_signals::SignalStatus::InvalidState);
    }
    if (injected_unsubscribe_failure_.has_value()) {
      const auto failure = *injected_unsubscribe_failure_;
      injected_unsubscribe_failure_.reset();
      return Result::failure(failure);
    }
    for (Slot &slot : slots_) {
      if (subscription.valid() && slot.bits == subscription.provider_bits()) {
        slot = Slot{};
        return Result::success();
      }
    }
    return Result::failure(vehicle_signals::SignalStatus::InvalidSubscription);
  }

  void start() noexcept { running_ = true; }
  void stop() noexcept { running_ = false; }
  [[nodiscard]] bool running() const noexcept { return running_; }

  // Makes the next unsubscribe() fail once with `status` (a failed or
  // timed-out provider mutation).
  void fail_next_unsubscribe(vehicle_signals::SignalStatus status) noexcept {
    injected_unsubscribe_failure_ = status;
  }

  // Delivers `notice` to the subscribers of notice.id; returns how many ran.
  std::size_t publish(const vehicle_signals::SignalNotification &notice) const noexcept {
    std::size_t delivered = 0;
    for (const Slot &slot : slots_) {
      if (running_ && slot.bits != 0 && slot.id == notice.id) {
        slot.callback(slot.context, notice);
        ++delivered;
      }
    }
    return delivered;
  }

  [[nodiscard]] std::size_t subscriber_count(vehicle_signals::SignalId id) const noexcept {
    std::size_t count = 0;
    for (const Slot &slot : slots_) {
      count += (slot.bits != 0 && slot.id == id) ? 1U : 0U;
    }
    return count;
  }
  [[nodiscard]] std::size_t subscription_count() const noexcept {
    std::size_t count = 0;
    for (const Slot &slot : slots_) {
      count += slot.bits != 0 ? 1U : 0U;
    }
    return count;
  }

private:
  struct ReadSlot {
    vehicle_signals::SignalId id{};
    vehicle_signals::SignalReading reading{};
    std::optional<vehicle_signals::SignalStatus> failure{};
  };

  [[nodiscard]] const ReadSlot *read_slot(vehicle_signals::SignalId id) const noexcept {
    for (const ReadSlot &slot : readings_) {
      if (slot.id.valid() && slot.id == id) {
        return &slot;
      }
    }
    return nullptr;
  }
  [[nodiscard]] ReadSlot *writable_read_slot(vehicle_signals::SignalId id) noexcept {
    for (ReadSlot &slot : readings_) {
      if (slot.id == id || !slot.id.valid()) {
        slot.id = id;
        return &slot;
      }
    }
    return nullptr;
  }

  struct Slot {
    vehicle_signals::SignalId id{};
    vehicle_signals::SignalCallback callback{nullptr};
    void *context{nullptr};
    std::uint64_t bits{0};
  };

  [[nodiscard]] vehicle_signals::SignalStatus
  subscribe_status(vehicle_signals::SignalId id,
                   vehicle_signals::SignalCallback callback) const noexcept {
    if (running_) {
      return vehicle_signals::SignalStatus::InvalidState;
    }
    if (callback == nullptr) {
      return vehicle_signals::SignalStatus::InvalidArgument;
    }
    const vehicle_signals::SignalMetadata *signal = catalog_.find(id);
    if (signal == nullptr) {
      return vehicle_signals::SignalStatus::InvalidSignal;
    }
    if (!signal->capabilities.has(vehicle_signals::SignalCapability::Notify)) {
      return vehicle_signals::SignalStatus::UnsupportedCapability;
    }
    if (subscriber_count(id) >= kSubscribersPerSignal) {
      return vehicle_signals::SignalStatus::CapacityExceeded;
    }
    return vehicle_signals::SignalStatus::Ok;
  }

  [[nodiscard]] Slot *free_slot() noexcept {
    for (Slot &slot : slots_) {
      if (slot.bits == 0) {
        return &slot;
      }
    }
    return nullptr;
  }

  vehicle_signals::SignalCatalogView catalog_{};
  std::array<Slot, kMaxSubscriptions> slots_{};
  std::array<ReadSlot, kMaxReadings> readings_{};
  std::uint64_t issued_{0};
  bool running_{false};
  std::optional<vehicle_signals::SignalStatus> injected_unsubscribe_failure_{};
};

} // namespace test_support
