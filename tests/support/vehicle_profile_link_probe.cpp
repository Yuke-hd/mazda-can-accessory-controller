#include <cstddef>
#include <cstdint>

#include "vehicle_profile_metadata.hpp"
#include "vehicle_profile_runtime.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/service.hpp"
#include "vehicle_profile_runtime/tables.hpp"
#include "vehicle_telemetry/receive.hpp"
#include "vehicle_core/time.hpp"

extern "C" {
extern volatile const std::uint32_t vehicle_profile_link_probe_service_implementation_bytes;
extern volatile const std::uint32_t
    vehicle_profile_link_probe_service_implementation_alignment;
}

namespace {

using vehicle_profile_runtime::ProfileTables;
using vehicle_profile_runtime::ProfileRuntimeService;
using vehicle_signals::SignalNotification;

constexpr ProfileTables kTables{
    vehicle_profile::kCatalog,
    vehicle_profile::kFreshnessPolicies.data(),
    vehicle_profile::kFreshnessPolicies.size(),
    vehicle_profile::kDispatchTable.data(),
    vehicle_profile::kDispatchTable.size(),
    vehicle_profile::kMaxDecodeResults,
};

class LinkProbeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override { return 0U; }
};

class LinkProbeSource final : public vehicle_telemetry::AcquisitionSource {
public:
  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    return {vehicle_telemetry::ResultCode::Faulted};
  }
  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    return {vehicle_telemetry::ResultCode::NotRunning};
  }
  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &, std::uint32_t) noexcept override {
    return vehicle_telemetry::ReceiveStatus::NotStarted;
  }
  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    return {};
  }
};

void discard_notification(void *, const SignalNotification &) noexcept {}

} // namespace

// This function is retained by -Wl,-u for link inspection and is never called
// by app_main. Its source cannot access a CAN driver or touch vehicle hardware.
extern "C" __attribute__((used, noinline)) void vehicle_profile_link_probe_anchor() {
  (void)vehicle_profile_link_probe_service_implementation_bytes;
  (void)vehicle_profile_link_probe_service_implementation_alignment;
  static LinkProbeClock clock{};
  static LinkProbeSource source{};
  static ProfileRuntimeService service{kTables, clock, source};

  auto &provider = service.provider();
  const auto catalog = provider.catalog();
  (void)catalog;
  (void)provider.read(vehicle_signals::SignalId{1U});
  (void)provider.subscribe(vehicle_signals::SignalId{1U}, discard_notification, nullptr);
  (void)provider.unsubscribe(vehicle_signals::SignalSubscription{});
  (void)service.start();
  (void)service.stop();
}
