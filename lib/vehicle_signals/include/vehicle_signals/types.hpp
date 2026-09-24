#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "vehicle_core/telemetry_contracts.hpp"

namespace vehicle_signals {

// Zero is reserved as the invalid/unassigned identifier for every catalog.
struct SignalId final {
  std::uint16_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

  friend constexpr bool operator==(SignalId lhs, SignalId rhs) noexcept {
    return lhs.value == rhs.value;
  }
  friend constexpr bool operator!=(SignalId lhs, SignalId rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(SignalId lhs, SignalId rhs) noexcept {
    return lhs.value < rhs.value;
  }
};

enum class SignalKind : std::uint8_t { Invalid, Boolean, Number, Enum };

// Units name common physical quantities without assigning any vehicle-specific
// meaning. Providers may use None when a signal is unitless.
enum class SignalUnit : std::uint8_t {
  None,
  Boolean,
  Percent,
  KilometresPerHour,
  MetresPerSecond,
  RevolutionsPerMinute,
  Celsius,
  Fahrenheit,
  Degrees,
  Seconds,
  Milliseconds,
  Count,
};

struct SignalEnumValue final {
  std::int32_t value{0};

  friend constexpr bool operator==(SignalEnumValue lhs, SignalEnumValue rhs) noexcept {
    return lhs.value == rhs.value;
  }
};

// A small, owning tagged value. Enum values keep their integer representation;
// labels and allowed values live in SignalMetadata and are borrowed there.
class SignalValue final {
public:
  constexpr SignalValue() noexcept = default;

  [[nodiscard]] static constexpr SignalValue boolean(bool value) noexcept {
    return SignalValue(Storage{std::in_place_index<1>, value});
  }
  [[nodiscard]] static SignalValue number(double value) noexcept {
    return SignalValue(Storage{std::in_place_index<2>, value});
  }
  [[nodiscard]] static constexpr SignalValue enumeration(std::int32_t value) noexcept {
    return SignalValue(Storage{std::in_place_index<3>, SignalEnumValue{value}});
  }

  [[nodiscard]] constexpr SignalKind kind() const noexcept {
    switch (value_.index()) {
    case 1:
      return SignalKind::Boolean;
    case 2:
      return SignalKind::Number;
    case 3:
      return SignalKind::Enum;
    default:
      return SignalKind::Invalid;
    }
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return kind() != SignalKind::Invalid; }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return kind() == SignalKind::Boolean; }
  [[nodiscard]] constexpr bool is_number() const noexcept { return kind() == SignalKind::Number; }
  [[nodiscard]] constexpr bool is_enum() const noexcept { return kind() == SignalKind::Enum; }

  [[nodiscard]] constexpr std::optional<bool> as_boolean() const noexcept {
    if (const auto *value = std::get_if<bool>(&value_)) {
      return *value;
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<double> as_number() const noexcept {
    if (const auto *value = std::get_if<double>(&value_)) {
      return *value;
    }
    return std::nullopt;
  }
  [[nodiscard]] constexpr std::optional<std::int32_t> as_enum() const noexcept {
    if (const auto *value = std::get_if<SignalEnumValue>(&value_)) {
      return value->value;
    }
    return std::nullopt;
  }

  friend bool operator==(const SignalValue &lhs, const SignalValue &rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const SignalValue &lhs, const SignalValue &rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  using Storage = std::variant<std::monostate, bool, double, SignalEnumValue>;

  constexpr explicit SignalValue(Storage value) noexcept : value_(value) {}

  Storage value_{};
};

// Preserve vehicle_core's value-only freshness and validation semantics while
// giving generic signal consumers a concise, signal-specific type name.
using SignalReading = vehicle_core::Reading<SignalValue>;
using SignalNotification = vehicle_core::Notification<SignalValue>;
using SignalAvailability = vehicle_core::Availability;
using SignalValidation = vehicle_core::ValidationStatus;

struct SignalChoice final {
  std::int32_t value{0};
  std::string_view label{};
};

struct SignalCapabilities final {
  bool readable{true};
  bool subscribable{true};
  bool writable{false};
};

struct SignalMetadata final {
  SignalId id{};
  std::string_view name{};
  std::string_view description{};
  SignalKind kind{SignalKind::Invalid};
  SignalUnit unit{SignalUnit::None};
  SignalValidation validation{SignalValidation::Reference};
  SignalCapabilities capabilities{};
  std::optional<double> minimum{};
  std::optional<double> maximum{};
  const SignalChoice *choices{nullptr};
  std::size_t choice_count{0};
};

enum class SignalStatus : std::uint8_t {
  Ok,
  InvalidSignal,
  UnsupportedCapability,
  InvalidCallback,
  CapacityExceeded,
  InvalidSubscription,
  InvalidState,
};

struct SignalReadResult final {
  SignalStatus status{SignalStatus::InvalidState};
  SignalReading reading{};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == SignalStatus::Ok; }
};

struct SignalSubscriptionToken final {
  SignalId signal{};
  std::uint16_t slot{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return signal.valid() && generation != 0; }

  friend constexpr bool operator==(const SignalSubscriptionToken &lhs,
                                   const SignalSubscriptionToken &rhs) noexcept {
    return lhs.signal == rhs.signal && lhs.slot == rhs.slot && lhs.generation == rhs.generation;
  }
  friend constexpr bool operator!=(const SignalSubscriptionToken &lhs,
                                   const SignalSubscriptionToken &rhs) noexcept {
    return !(lhs == rhs);
  }
};

struct SignalSubscriptionResult final {
  SignalStatus status{SignalStatus::InvalidState};
  SignalSubscriptionToken token{};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return status == SignalStatus::Ok && token.valid();
  }
};

// Callback context is borrowed. Keep it alive until a successful provider
// stop has quiesced every callback, and copy the notification if retaining it
// beyond this invocation. Configuration and subscription changes are made
// only while the provider is stopped.
using SignalCallback = void (*)(void *context, SignalId signal,
                                const SignalNotification &notification) noexcept;

} // namespace vehicle_signals
