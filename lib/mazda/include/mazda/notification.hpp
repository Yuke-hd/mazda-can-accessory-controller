#pragma once

#include "mazda/reading.hpp"
#include "vehicle_core/notification.hpp"

namespace mazda {

// Notifications remain value-copy contracts. The callback alias is repeated
// in the Mazda namespace to preserve the existing qualified API without
// importing the notification-channel implementation.
using vehicle_core::Callback;
using vehicle_core::Notification;

} // namespace mazda
