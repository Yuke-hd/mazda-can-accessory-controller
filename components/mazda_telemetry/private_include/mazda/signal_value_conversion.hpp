#pragma once

#include <cstdint>
#include <type_traits>

#include "mazda/notification.hpp"
#include "mazda/reading.hpp"
#include "mazda/signal_catalog.hpp"
#include "vehicle_core/signal.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda::internal {

// Implementation-only conversions from typed Mazda readings/notifications to
// the value-only vehicle_signals contracts. They are shared by the provider's
// read adapter and notification bridge so both paths produce identical
// generic values. Conversions never rescale a value or reinterpret state:
// has_value, availability and validation are copied exactly.

// Generic type of a typed Mazda value: bool is Boolean, float is Number and a
// Mazda enum is Enum.
template <typename T>
[[nodiscard]] constexpr vehicle_signals::SignalType signal_type_of() noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return vehicle_signals::SignalType::Boolean;
  } else if constexpr (std::is_same_v<T, float>) {
    return vehicle_signals::SignalType::Number;
  } else {
    static_assert(std::is_enum_v<T>, "only bool, float and Mazda enum values are convertible");
    return vehicle_signals::SignalType::Enum;
  }
}

// Enum values use the raw enumerator value, the same encoding the catalog's
// enum choices use (choice_value in signal_catalog.hpp).
template <typename T>
[[nodiscard]] constexpr vehicle_signals::SignalValue to_signal_value(const T value) noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return vehicle_signals::SignalValue::boolean(value);
  } else if constexpr (std::is_same_v<T, float>) {
    return vehicle_signals::SignalValue::number(value);
  } else {
    static_assert(std::is_enum_v<T>, "only bool, float and Mazda enum values are convertible");
    static_assert(sizeof(std::underlying_type_t<T>) <= sizeof(std::uint16_t) &&
                      std::is_unsigned_v<std::underlying_type_t<T>>,
                  "Mazda enum values must fit an unsigned 16-bit choice value");
    return vehicle_signals::SignalValue::enumeration(choice_value(value));
  }
}

template <typename T>
[[nodiscard]] constexpr vehicle_signals::SignalReading
to_signal_reading(const Reading<T> &reading) noexcept {
  vehicle_signals::SignalReading result{};
  if (reading.value.has_value())
    result.value = to_signal_value(*reading.value);
  result.availability = reading.availability;
  result.validation = reading.validation;
  return result;
}

// Copies `current` and exactly the four notification flags; no previous
// reading is reconstructed.
template <typename T>
[[nodiscard]] constexpr vehicle_signals::SignalNotification
to_signal_notification(const vehicle_signals::SignalId id,
                       const Notification<T> &notification) noexcept {
  vehicle_signals::SignalNotification result{};
  result.id = id;
  result.current = to_signal_reading(notification.current);
  result.initial = notification.initial;
  result.became_unavailable = notification.became_unavailable;
  result.recovered = notification.recovered;
  result.coalesced = notification.coalesced;
  return result;
}

// The only Mazda-to-generic unit mapping. A reading value is never rescaled;
// the catalog row carries the unit, and Boolean state has no generic unit.
[[nodiscard]] constexpr vehicle_signals::SignalUnit
to_signal_unit(const vehicle_core::SignalUnit unit) noexcept {
  switch (unit) {
  case vehicle_core::SignalUnit::KilometresPerHour:
    return vehicle_signals::SignalUnit::KilometresPerHour;
  case vehicle_core::SignalUnit::RevolutionsPerMinute:
    return vehicle_signals::SignalUnit::RevolutionsPerMinute;
  case vehicle_core::SignalUnit::None:
  case vehicle_core::SignalUnit::Boolean:
    return vehicle_signals::SignalUnit::None;
  }
  return vehicle_signals::SignalUnit::None;
}

// Catalog request check shared by read and subscribe: an invalid or unknown
// id is InvalidSignal, and a row without the required capability is
// UnsupportedCapability. On success the result holds the catalog row.
[[nodiscard]] constexpr vehicle_signals::SignalResult<const vehicle_signals::SignalMetadata *>
find_catalog_signal(const vehicle_signals::SignalCatalogView catalog,
                    const vehicle_signals::SignalId id,
                    const vehicle_signals::SignalCapability required) noexcept {
  using Result = vehicle_signals::SignalResult<const vehicle_signals::SignalMetadata *>;
  const vehicle_signals::SignalMetadata *metadata = catalog.find(id);
  if (metadata == nullptr)
    return Result::failure(vehicle_signals::SignalStatus::InvalidSignal);
  if (!metadata->capabilities.has(required))
    return Result::failure(vehicle_signals::SignalStatus::UnsupportedCapability);
  return Result::success(metadata);
}

} // namespace mazda::internal
