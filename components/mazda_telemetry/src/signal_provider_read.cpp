#include "mazda/signal_provider.hpp"

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_value_conversion.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <optional>
#include <tuple>

namespace mazda {

namespace {

using vehicle_signals::SignalId;
using vehicle_signals::SignalReading;

// Reads the descriptor bound to `id` through the service's coherent
// production descriptor read. Host-only test descriptors keep the invalid id
// and are never matched.
template <typename Descriptor>
bool read_if_bound(const internal::VehicleTelemetryService &service, const Descriptor &descriptor,
                   const SignalId id, std::optional<SignalReading> &reading) noexcept {
  if (!descriptor.id.valid() || descriptor.id != id)
    return false;
  reading = internal::to_signal_reading(service.read_descriptor(descriptor));
  return true;
}

template <typename Tuple>
bool read_bound_descriptor(const internal::VehicleTelemetryService &service,
                           const Tuple &descriptors, const SignalId id,
                           std::optional<SignalReading> &reading) noexcept {
  return std::apply(
      [&service, id, &reading](const auto &...descriptor) noexcept {
        return (read_if_bound(service, descriptor, id, reading) || ...);
      },
      descriptors);
}

} // namespace

vehicle_signals::SignalResult<SignalReading>
MazdaSignalProvider::read(const SignalId id) const noexcept {
  using Result = vehicle_signals::SignalResult<SignalReading>;
  const auto lookup = internal::find_catalog_signal(internal::signal_catalog(), id,
                                                    vehicle_signals::SignalCapability::Read);
  if (!lookup.ok())
    return Result::failure(lookup.status);

  // A successful read returns the reading as published, including NoData,
  // Stale and Unavailable availability; only request failures use a status.
  const VehicleTelemetry &telemetry = *telemetry_;
  const internal::VehicleTelemetryService &service =
      internal::VehicleTelemetryAccess::service(telemetry);
  std::optional<SignalReading> reading{};
  if (read_bound_descriptor(service, internal::VehicleTelemetryService::polling_descriptors(), id,
                            reading) ||
      read_bound_descriptor(service, internal::VehicleTelemetryService::notification_descriptors(),
                            id, reading))
    return Result::success(*reading);

  // Unreachable with the released catalog: every Read row is bound to exactly
  // one production descriptor (signal_catalog_tests). An unbound row cannot be
  // read, so it is reported as lacking the capability rather than as NoData.
  return Result::failure(vehicle_signals::SignalStatus::UnsupportedCapability);
}

} // namespace mazda
