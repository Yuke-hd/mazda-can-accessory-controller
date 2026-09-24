#pragma once

#include <cstdint>
#include <optional>

#include "vehicle_core/telemetry_contracts.hpp"

// Portable, value-only signal contracts. This header knows no vehicle make,
// CAN frame, decoder, notification channel, RTOS, or driver. Providers that
// bind these contracts to concrete telemetry live in their own components.

namespace vehicle_signals {

using vehicle_core::Availability;
using vehicle_core::ValidationStatus;

// Opaque runtime signal identifier. Numeric ids are per-build runtime details
// assigned by a catalog; they may change between firmware builds and must not
// be persisted. Persistent configuration refers to signals by their catalog
// string key instead. Zero is reserved as the invalid id.
class SignalId final {
public:
  constexpr SignalId() noexcept = default;
  constexpr explicit SignalId(std::uint16_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr std::uint16_t value() const noexcept { return value_; }

  friend constexpr bool operator==(SignalId left, SignalId right) noexcept {
    return left.value_ == right.value_;
  }
  friend constexpr bool operator!=(SignalId left, SignalId right) noexcept {
    return !(left == right);
  }
  friend constexpr bool operator<(SignalId left, SignalId right) noexcept {
    return left.value_ < right.value_;
  }

private:
  std::uint16_t value_{0};
};

enum class SignalType : std::uint8_t { Boolean, Number, Enum };

// Engineering unit of a Number signal. Boolean and Enum signals use None.
enum class SignalUnit : std::uint8_t { None, KilometresPerHour, RevolutionsPerMinute };

// Tagged Boolean/Number/Enum value. Typed accessors return std::nullopt on a
// type mismatch instead of reinterpreting storage. Enum values are the raw
// choice values described by the signal's catalog metadata. A default value is
// Boolean false. Number equality follows float comparison (NaN is unequal).
class SignalValue final {
public:
  constexpr SignalValue() noexcept = default;

  [[nodiscard]] static constexpr SignalValue boolean(bool value) noexcept {
    SignalValue result{};
    result.type_ = SignalType::Boolean;
    result.boolean_ = value;
    return result;
  }
  [[nodiscard]] static constexpr SignalValue number(float value) noexcept {
    SignalValue result{};
    result.type_ = SignalType::Number;
    result.number_ = value;
    return result;
  }
  [[nodiscard]] static constexpr SignalValue enumeration(std::uint16_t value) noexcept {
    SignalValue result{};
    result.type_ = SignalType::Enum;
    result.enumeration_ = value;
    return result;
  }

  [[nodiscard]] constexpr SignalType type() const noexcept { return type_; }

  [[nodiscard]] constexpr std::optional<bool> as_boolean() const noexcept {
    if (type_ != SignalType::Boolean) {
      return std::nullopt;
    }
    return boolean_;
  }
  [[nodiscard]] constexpr std::optional<float> as_number() const noexcept {
    if (type_ != SignalType::Number) {
      return std::nullopt;
    }
    return number_;
  }
  [[nodiscard]] constexpr std::optional<std::uint16_t> as_enumeration() const noexcept {
    if (type_ != SignalType::Enum) {
      return std::nullopt;
    }
    return enumeration_;
  }

  friend constexpr bool operator==(const SignalValue &left, const SignalValue &right) noexcept {
    if (left.type_ != right.type_) {
      return false;
    }
    switch (left.type_) {
    case SignalType::Boolean:
      return left.boolean_ == right.boolean_;
    case SignalType::Number:
      return left.number_ == right.number_;
    case SignalType::Enum:
      return left.enumeration_ == right.enumeration_;
    }
    return false;
  }
  friend constexpr bool operator!=(const SignalValue &left, const SignalValue &right) noexcept {
    return !(left == right);
  }

private:
  SignalType type_{SignalType::Boolean};
  bool boolean_{false};
  std::uint16_t enumeration_{0};
  float number_{0.0F};
};

// Mirrors vehicle_core::Reading: availability and validation are independent
// of whether a value is present. A successful request can legitimately return
// Availability::NoData; request failures are reported by SignalStatus instead.
struct SignalReading {
  std::optional<SignalValue> value{};
  Availability availability{Availability::NoData};
  ValidationStatus validation{ValidationStatus::Reference};
};

// Latest-state notice for one signal, matching vehicle_core::Notification.
// `current` is the latest reading; no previous reading is reconstructed. A
// reading is usable when it has a value and is Fresh or FreshnessUnverified.
// Flags are sticky across coalescing:
// - initial: the seed notice each provider start creates per subscription.
// - became_unavailable: a usable reading turned unusable since the last notice.
// - recovered: a reading became usable again after an earlier usable period in
//   the same run.
// - coalesced: intermediate updates were merged into this latest-state notice.
struct SignalNotification {
  SignalId id{};
  SignalReading current{};
  bool initial{false};
  bool became_unavailable{false};
  bool recovered{false};
  bool coalesced{false};
};

// Request outcome, independent of reading availability.
enum class SignalStatus : std::uint8_t {
  Ok,
  InvalidSignal,         // Invalid or unknown SignalId.
  UnsupportedCapability, // The signal lacks the requested capability.
  InvalidArgument,       // For example a null callback.
  CapacityExceeded,
  InvalidState, // For example subscription mutation while running.
  InvalidSubscription,
  Faulted,
  Timeout,
};

// ok() holds exactly when status is Ok and a value is present.
template <typename T> struct SignalResult {
  SignalStatus status{SignalStatus::InvalidState};
  std::optional<T> value{};

  [[nodiscard]] static constexpr SignalResult success(T result) noexcept {
    return SignalResult{SignalStatus::Ok, std::optional<T>{result}};
  }
  // A failure never carries a value; Ok is mapped to InvalidState.
  [[nodiscard]] static constexpr SignalResult failure(SignalStatus failure_status) noexcept {
    return SignalResult{failure_status == SignalStatus::Ok ? SignalStatus::InvalidState
                                                           : failure_status,
                        std::nullopt};
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] constexpr bool ok() const noexcept {
    return status == SignalStatus::Ok && value.has_value();
  }
};

template <> struct SignalResult<void> {
  SignalStatus status{SignalStatus::InvalidState};

  [[nodiscard]] static constexpr SignalResult success() noexcept {
    return SignalResult{SignalStatus::Ok};
  }
  [[nodiscard]] static constexpr SignalResult failure(SignalStatus failure_status) noexcept {
    return SignalResult{failure_status == SignalStatus::Ok ? SignalStatus::InvalidState
                                                           : failure_status};
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] constexpr bool ok() const noexcept { return status == SignalStatus::Ok; }
};

using SignalStatusResult = SignalResult<void>;

// Provider callback contract:
// - Callbacks receive value-only notification copies and may run on a
//   provider-owned context.
// - The context pointer and any pointed-to storage are borrowed until the
//   provider's stop succeeds; a failed or timed-out stop leaves callbacks
//   potentially active.
// - A callback must not start, stop, subscribe, or unsubscribe on the provider
//   that invoked it.
// - Subscriptions are created and removed only while the provider is stopped;
//   mutation while running is rejected with SignalStatus::InvalidState.
using SignalCallback = void (*)(void *context, const SignalNotification &notification) noexcept;

// Opaque subscription token. Consumers must treat it as opaque and only store,
// compare, and return it to the provider that issued it. The concrete provider
// encodes its own handle in the raw bits; zero is reserved as invalid, so a
// default token is never a live subscription. valid() only reports a non-zero
// encoding and does not prove the provider still holds the registration.
class SignalSubscription final {
public:
  constexpr SignalSubscription() noexcept = default;

  // Provider-only encode/decode seam.
  [[nodiscard]] static constexpr SignalSubscription
  from_provider_bits(std::uint64_t bits) noexcept {
    SignalSubscription result{};
    result.bits_ = bits;
    return result;
  }
  [[nodiscard]] constexpr std::uint64_t provider_bits() const noexcept { return bits_; }

  [[nodiscard]] constexpr bool valid() const noexcept { return bits_ != 0; }

  friend constexpr bool operator==(SignalSubscription left, SignalSubscription right) noexcept {
    return left.bits_ == right.bits_;
  }
  friend constexpr bool operator!=(SignalSubscription left, SignalSubscription right) noexcept {
    return !(left == right);
  }

private:
  std::uint64_t bits_{0};
};

enum class SignalCapability : std::uint8_t {
  Read = 1U << 0U,
  Notify = 1U << 1U,
};

// Small capability bitmask. Unknown bits are discarded.
class SignalCapabilities final {
public:
  constexpr SignalCapabilities() noexcept = default;
  constexpr SignalCapabilities(SignalCapability capability) noexcept // NOLINT: implicit by design
      : bits_(static_cast<std::uint8_t>(static_cast<std::uint8_t>(capability) & kKnownBits)) {}

  [[nodiscard]] static constexpr SignalCapabilities from_bits(std::uint8_t bits) noexcept {
    SignalCapabilities result{};
    result.bits_ = static_cast<std::uint8_t>(bits & kKnownBits);
    return result;
  }
  [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

  [[nodiscard]] constexpr bool has(SignalCapability capability) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(capability)) != 0;
  }
  // True when every capability in `required` is present.
  [[nodiscard]] constexpr bool supports(SignalCapabilities required) const noexcept {
    return (bits_ & required.bits_) == required.bits_;
  }

  friend constexpr SignalCapabilities operator|(SignalCapabilities left,
                                                SignalCapabilities right) noexcept {
    return from_bits(static_cast<std::uint8_t>(left.bits_ | right.bits_));
  }
  friend constexpr bool operator==(SignalCapabilities left, SignalCapabilities right) noexcept {
    return left.bits_ == right.bits_;
  }
  friend constexpr bool operator!=(SignalCapabilities left, SignalCapabilities right) noexcept {
    return !(left == right);
  }

private:
  static constexpr std::uint8_t kKnownBits = static_cast<std::uint8_t>(SignalCapability::Read) |
                                             static_cast<std::uint8_t>(SignalCapability::Notify);
  std::uint8_t bits_{0};
};

[[nodiscard]] constexpr SignalCapabilities operator|(SignalCapability left,
                                                     SignalCapability right) noexcept {
  return SignalCapabilities{left} | SignalCapabilities{right};
}

} // namespace vehicle_signals
