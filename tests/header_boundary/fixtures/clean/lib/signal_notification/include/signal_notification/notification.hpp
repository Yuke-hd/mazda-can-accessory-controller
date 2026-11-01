#pragma once

#include "vehicle_core/telemetry_contracts.hpp"

namespace signal_notification {

template <typename T> struct Notification { vehicle_core::Reading<T> current{}; };

template <typename T>
using Callback = void (*)(void *context, const Notification<T> &notification) noexcept;

} // namespace signal_notification
