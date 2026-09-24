#include "mazda/vehicle_telemetry_service.hpp"

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/signal_provider_internal.hpp"
#include "mazda/state.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"

namespace mazda::internal {
namespace {

template <typename T> vehicle_signals::SignalValue signal_value(const T value) noexcept {
  using Value = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<Value, bool>) {
    return vehicle_signals::SignalValue::boolean(value);
  } else if constexpr (std::is_enum_v<Value>) {
    return vehicle_signals::SignalValue::enumeration(static_cast<std::int32_t>(value));
  } else {
    static_assert(std::is_arithmetic_v<Value>, "unsupported Mazda signal value type");
    return vehicle_signals::SignalValue::number(static_cast<double>(value));
  }
}

template <typename T>
vehicle_signals::SignalReadResult generic_reading(const Reading<T> &reading) noexcept {
  vehicle_signals::SignalReadResult result{};
  result.status = vehicle_signals::SignalStatus::Ok;
  result.reading.availability = reading.availability;
  result.reading.validation = reading.validation;
  if (reading.value.has_value())
    result.reading.value = signal_value(*reading.value);
  return result;
}

template <typename Descriptor>
bool read_if_matched(const Descriptor &descriptor, const vehicle_signals::SignalId signal_id,
                     const PublicationStore &publication,
                     vehicle_signals::SignalReadResult &result) noexcept {
  if (!signal_id.valid() || descriptor.signal_id != signal_id)
    return false;

  result = generic_reading(
      publication.read_descriptor(descriptor.signal, descriptor.identifier, descriptor.validation));
  return true;
}

template <typename Tuple>
std::size_t read_from_descriptors(const Tuple &descriptors,
                                  const vehicle_signals::SignalId signal_id,
                                  const PublicationStore &publication,
                                  vehicle_signals::SignalReadResult &result) noexcept {
  std::size_t matches = 0;
  std::apply(
      [&publication, signal_id, &result, &matches](const auto &...descriptor) {
        ((matches +=
          static_cast<std::size_t>(read_if_matched(descriptor, signal_id, publication, result))),
         ...);
      },
      descriptors);
  return matches;
}

} // namespace

vehicle_signals::SignalReadResult
VehicleTelemetryService::read_signal(const vehicle_signals::SignalId signal_id) const noexcept {
  const auto *metadata = signals::catalog().find(signal_id);
  if (metadata == nullptr)
    return {vehicle_signals::SignalStatus::InvalidSignal, {}};
  if (!metadata->capabilities.readable)
    return {vehicle_signals::SignalStatus::UnsupportedCapability, {}};

  vehicle_signals::SignalReadResult result{};
  auto matches = read_from_descriptors(polling_descriptors(), signal_id, publication_, result);
  matches += read_from_descriptors(notification_descriptors(), signal_id, publication_, result);
  if (matches == 1)
    return result;
  if (matches > 1)
    return {vehicle_signals::SignalStatus::InvalidState, {}};
  return {vehicle_signals::SignalStatus::UnsupportedCapability, {}};
}

} // namespace mazda::internal

namespace mazda::internal {

VehicleTelemetryService &VehicleTelemetryAccess::service(VehicleTelemetry &facade) noexcept {
  return *reinterpret_cast<VehicleTelemetryService *>(facade.implementation_storage_);
}

#if !defined(ESP_PLATFORM)
MazdaSignalProvider
VehicleTelemetryAccess::for_host_test(VehicleTelemetryService &service) noexcept {
  return MazdaSignalProvider{service};
}
#endif

} // namespace mazda::internal

namespace mazda {

MazdaSignalProvider::MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept
    : telemetry_(&telemetry), service_(&internal::VehicleTelemetryAccess::service(telemetry)) {
  static_assert(sizeof(Impl) <= kImplementationStorageBytes,
                "MazdaSignalProvider implementation storage is too small");
  static_assert(alignof(Impl) <= alignof(std::max_align_t),
                "MazdaSignalProvider implementation alignment is too large");
  ::new (static_cast<void *>(implementation_storage_)) Impl{*service_};
}

MazdaSignalProvider::MazdaSignalProvider(internal::VehicleTelemetryService &service) noexcept
    : service_(&service) {
  static_assert(sizeof(Impl) <= kImplementationStorageBytes,
                "MazdaSignalProvider implementation storage is too small");
  static_assert(alignof(Impl) <= alignof(std::max_align_t),
                "MazdaSignalProvider implementation alignment is too large");
  ::new (static_cast<void *>(implementation_storage_)) Impl{service};
}

MazdaSignalProvider::~MazdaSignalProvider() noexcept {
  reinterpret_cast<Impl *>(implementation_storage_)->~Impl();
}

vehicle_signals::SignalCatalogView MazdaSignalProvider::catalog() const noexcept {
  return internal::signals::catalog();
}

vehicle_signals::SignalReadResult
MazdaSignalProvider::read(const vehicle_signals::SignalId signal_id) const noexcept {
  if (service_ == nullptr)
    return {vehicle_signals::SignalStatus::InvalidState, {}};
  return service_->read_signal(signal_id);
}

} // namespace mazda
