#pragma once

#include <cstdint>

#include "vehicle_core/telemetry_contracts.hpp"

namespace vehicle_signals {

class SignalId final {
public:
  constexpr SignalId() noexcept = default;
  constexpr explicit SignalId(std::uint16_t value) noexcept : value_(value) {}

private:
  std::uint16_t value_{0};
};

struct SignalReading {};

enum class SignalStatus : std::uint8_t { Ok, InvalidSignal };

template <typename T> struct SignalResult {
  SignalStatus status{SignalStatus::InvalidSignal};

  [[nodiscard]] static constexpr SignalResult failure(SignalStatus failure_status) noexcept {
    return SignalResult{failure_status};
  }
  [[nodiscard]] constexpr bool ok() const noexcept { return status == SignalStatus::Ok; }
};

} // namespace vehicle_signals
