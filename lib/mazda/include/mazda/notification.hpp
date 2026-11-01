#pragma once

#include "mazda/reading.hpp"
#include "signal_notification/notification.hpp"

namespace mazda {

// Notifications remain value-copy contracts. The callback alias is repeated
// in the Mazda namespace to preserve the existing qualified API without
// importing the notification-channel implementation.
using signal_notification::Callback;
using signal_notification::Notification;

} // namespace mazda
