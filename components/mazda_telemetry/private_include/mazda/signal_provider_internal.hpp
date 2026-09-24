#pragma once

#include <array>
#include <cstddef>

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

namespace mazda {

// Fixed storage behind the public provider. A typed channel receives a
// pointer to one of these records as its context; the trampoline converts the
// value-only typed notification and calls the generic client callback.
class MazdaSignalProvider::Impl final {
public:
  using TypedSubscription = internal::SubscriptionToken;

  struct CallbackRecord final {
    vehicle_signals::SignalId signal{};
    vehicle_signals::SignalCallback callback{nullptr};
    void *context{nullptr};
    TypedSubscription typed_subscription{};
    bool active{false};
  };

  explicit Impl(internal::VehicleTelemetryService &service) noexcept;
  ~Impl() noexcept;

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  [[nodiscard]] vehicle_signals::SignalSubscriptionResult
  subscribe(vehicle_signals::SignalId signal, vehicle_signals::SignalCallback callback,
            void *context) noexcept;
  [[nodiscard]] vehicle_signals::SignalStatus
  unsubscribe(vehicle_signals::SignalSubscriptionToken token) noexcept;

  static constexpr std::size_t kCallbackCapacity =
      internal::signals::kSignalCount * vehicle_core::kNotificationSubscribersPerChannel;

private:
  template <typename T>
  static void callback_trampoline(void *context,
                                  const vehicle_core::Notification<T> &notification) noexcept;

  template <typename T, std::uint16_t ChannelId>
  [[nodiscard]] static bool
  subscribe_descriptor(internal::VehicleTelemetryService &service,
                       const internal::NotificationDescriptor<T, ChannelId> &descriptor,
                       vehicle_signals::SignalId signal, CallbackRecord &record,
                       TypedSubscription &subscription) noexcept;

  [[nodiscard]] CallbackRecord *free_record() noexcept;
  [[nodiscard]] CallbackRecord *
  find_record(vehicle_signals::SignalSubscriptionToken token) noexcept;

  internal::VehicleTelemetryService *service_{nullptr};
  std::array<CallbackRecord, kCallbackCapacity> callbacks_{};
};

} // namespace mazda
