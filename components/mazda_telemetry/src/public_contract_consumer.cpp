#include "mazda/facade_contracts.hpp"
#include "mazda/notification.hpp"
#include "mazda/reading.hpp"
#include "mazda/telemetry_contracts.hpp"
#include "mazda/types.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "vehicle_core/telemetry_contracts.hpp"

#include <type_traits>

namespace {

using Facade = mazda::VehicleTelemetry;
using ConfigureFn = mazda::StatusResult (Facade::*)(const mazda::TelemetryConfig &) noexcept;
using PollingFn = mazda::Reading<float> (Facade::*)() const noexcept;

static_assert(std::is_same_v<decltype(&Facade::configure), ConfigureFn>);
static_assert(std::is_same_v<decltype(&Facade::speed_kph), PollingFn>);
static_assert(std::is_same_v<decltype(&Facade::engine_rpm), PollingFn>);
static_assert(std::is_trivially_copyable_v<mazda::Reading<float>>);
static_assert(std::is_trivially_copyable_v<mazda::Notification<bool>>);

} // namespace

extern "C" void vehicle_telemetry_public_contract_consumer_probe() noexcept {}
