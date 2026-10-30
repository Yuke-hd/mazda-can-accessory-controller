#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "vehicle_core/notification_channel.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_profile_runtime {

struct ProfileState;

inline constexpr std::size_t kMaxNotifyBindings = 17;
inline constexpr std::size_t kNotifySubscribersPerBinding =
    vehicle_core::kNotificationSubscribersPerChannel;

// ChannelId is a compile-time type parameter in vehicle_core. Each generated
// Notify catalog entry gets its own fixed channel instance; the provider token
// carries that runtime catalog index in addition to this primitive's slot and
// generation.
using RuntimeNotificationChannel =
    vehicle_core::NotificationChannel<vehicle_signals::SignalValue, 0>;

struct NotificationRegistration final {
  vehicle_signals::SignalId id{};
  vehicle_signals::SignalCallback callback{nullptr};
  void *context{nullptr};
  std::atomic<bool> *callback_active{nullptr};
  vehicle_core::NotificationHandle channel_handle{};
  std::uint16_t generation{0};
  bool active{false};
};

struct NotificationChannelBinding final {
  vehicle_signals::SignalId id{};
  RuntimeNotificationChannel channel{};
  std::array<NotificationRegistration, kNotifySubscribersPerBinding> registrations{};
};

struct NotificationState final {
  std::array<NotificationChannelBinding, kMaxNotifyBindings> channels{};
  std::size_t channel_count{0};
  std::atomic<bool> running{false};
  std::atomic<bool> callback_active{false};
};

void initialize_notification_channels(ProfileState &state) noexcept;

} // namespace vehicle_profile_runtime
