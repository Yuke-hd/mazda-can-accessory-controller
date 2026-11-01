#pragma once

// Value-copy notification contract shared by this repository's signal
// providers. It carries only the latest Reading and transition evidence; the
// delivery implementation lives in the provider-private
// signal_notification/notification_channel.hpp header.
//
// Provenance: derived from the Notification<T> and Callback<T> declarations in
// components/vehicle_core/include/vehicle_core/telemetry_contracts.hpp of
// Yuke-hd/esp32-vehicle-can-core release 0.2.1
// (b3d87ac094576f7a787114a1bd24db5cf427a4f7), Apache-2.0. Core 0.3.0 removed
// them; this repository now owns the contract.

#include "vehicle_core/telemetry_contracts.hpp"

namespace signal_notification {

template <typename T> struct Notification {
  vehicle_core::Reading<T> current{};
  bool initial{false};
  bool became_unavailable{false};
  bool recovered{false};
  bool coalesced{false};
};

template <typename T>
using Callback = void (*)(void *context, const Notification<T> &notification) noexcept;

} // namespace signal_notification
