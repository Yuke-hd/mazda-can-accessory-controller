#pragma once

#include <cstdint>
#include <memory>

#include "vehicle_profile_runtime/provider.hpp"

namespace vehicle_profile_runtime {

enum class ReplayAdapterStatus : std::uint8_t { Ok, InvalidState, Faulted };

// Synchronous host adapter for deterministic replay. Every operation and
// provider subscription mutation must run on the construction thread.
class ReplayAdapter final {
public:
  ReplayAdapter(ProfileTables tables, vehicle_core::MonotonicClock &clock) noexcept;
  ~ReplayAdapter() noexcept;

  ReplayAdapter(const ReplayAdapter &) = delete;
  ReplayAdapter &operator=(const ReplayAdapter &) = delete;
  ReplayAdapter(ReplayAdapter &&) = delete;
  ReplayAdapter &operator=(ReplayAdapter &&) = delete;

  [[nodiscard]] vehicle_signals::SignalProvider &provider() noexcept;
  [[nodiscard]] ReplayAdapterStatus start() noexcept;
  [[nodiscard]] ReplayAdapterStatus process(const vehicle_core::RawCanFrame &frame) noexcept;
  [[nodiscard]] ReplayAdapterStatus service() noexcept;
  [[nodiscard]] ReplayAdapterStatus source_fault() noexcept;
  [[nodiscard]] ReplayAdapterStatus stop() noexcept;

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace vehicle_profile_runtime
