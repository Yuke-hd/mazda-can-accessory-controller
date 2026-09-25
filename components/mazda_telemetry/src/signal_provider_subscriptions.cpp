#include "mazda/signal_provider.hpp"

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_value_conversion.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <cstdint>
#include <tuple>

namespace mazda {

namespace {

using vehicle_signals::SignalId;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalStatusResult;
using vehicle_signals::SignalSubscription;

// Generic token layout (the typed SubscriptionToken fields, which keep the
// channel generation and therefore stale-token protection):
//   bits  0..15 generation (never zero for a registration)
//   bits 16..23 slot
//   bits 32..47 channel
// Every other bit is zero, so a successful token is never the invalid zero.
// Tokens are facade-scoped values: the service matches them exactly against
// its own live generic records.
constexpr std::uint64_t kGenerationMask = 0xffffU;
constexpr std::uint64_t kSlotMask = 0xffU;
constexpr std::uint64_t kChannelMask = 0xffffU;
constexpr unsigned kSlotShift = 16U;
constexpr unsigned kChannelShift = 32U;
constexpr std::uint64_t kUsedBits =
    kGenerationMask | (kSlotMask << kSlotShift) | (kChannelMask << kChannelShift);

[[nodiscard]] constexpr SignalSubscription
encode(const internal::SubscriptionToken &token) noexcept {
  return SignalSubscription::from_provider_bits((std::uint64_t{token.channel} << kChannelShift) |
                                                (std::uint64_t{token.slot} << kSlotShift) |
                                                std::uint64_t{token.generation});
}

[[nodiscard]] constexpr internal::SubscriptionToken
decode(const SignalSubscription subscription) noexcept {
  const auto bits = subscription.provider_bits();
  return {ResultCode::Ok, static_cast<std::uint16_t>((bits >> kChannelShift) & kChannelMask),
          static_cast<std::uint8_t>((bits >> kSlotShift) & kSlotMask),
          static_cast<std::uint16_t>(bits & kGenerationMask)};
}

[[nodiscard]] constexpr SignalStatus map_status(const ResultCode status) noexcept {
  switch (status) {
  case ResultCode::Ok:
    // Callers map only failures; Ok here would be a bridge defect.
    return SignalStatus::InvalidState;
  case ResultCode::CapacityExceeded:
    return SignalStatus::CapacityExceeded;
  case ResultCode::InvalidSubscription:
    return SignalStatus::InvalidSubscription;
  case ResultCode::InvalidState:
  case ResultCode::AlreadyRunning:
  case ResultCode::NotRunning:
  case ResultCode::InvalidConfiguration:
    return SignalStatus::InvalidState;
  case ResultCode::Faulted:
    return SignalStatus::Faulted;
  case ResultCode::Timeout:
    return SignalStatus::Timeout;
  }
  return SignalStatus::InvalidState;
}

// Registers a generic subscription on the production typed channel bound to
// `id`. A Notify id without a descriptor keeps the default InvalidState token;
// the catalog/descriptor binding tests make that unreachable.
[[nodiscard]] internal::SubscriptionToken
subscribe_bound_descriptor(internal::VehicleTelemetryService &service, const SignalId id,
                           const vehicle_signals::SignalCallback callback,
                           void *const context) noexcept {
  internal::SubscriptionToken token{};
  bool found = false;
  std::apply(
      [&](const auto &...descriptor) {
        (([&] {
           if (found || !descriptor.id.valid() || descriptor.id != id)
             return;
           found = true;
           token = service.subscribe_generic_descriptor(descriptor, callback, context);
         }()),
         ...);
      },
      internal::VehicleTelemetryService::notification_descriptors());
  return token;
}

} // namespace

vehicle_signals::SignalResult<SignalSubscription>
MazdaSignalProvider::subscribe(const SignalId id, const vehicle_signals::SignalCallback callback,
                               void *const context) noexcept {
  using Result = vehicle_signals::SignalResult<SignalSubscription>;
  const auto lookup = internal::find_catalog_signal(internal::signal_catalog(), id,
                                                    vehicle_signals::SignalCapability::Notify);
  if (!lookup.ok())
    return Result::failure(lookup.status);
  if (callback == nullptr)
    return Result::failure(SignalStatus::InvalidArgument);

  const auto token = subscribe_bound_descriptor(
      internal::VehicleTelemetryAccess::service(*telemetry_), id, callback, context);
  if (!token.ok())
    return Result::failure(map_status(token.status));
  return Result::success(encode(token));
}

vehicle_signals::SignalStatusResult
MazdaSignalProvider::unsubscribe(const SignalSubscription subscription) noexcept {
  // A malformed encoding can never match a live generic record. Anything else
  // is decoded and matched exactly by the service, which never lets a generic
  // token remove a typed registration.
  if (!subscription.valid() || (subscription.provider_bits() & ~kUsedBits) != 0)
    return SignalStatusResult::failure(SignalStatus::InvalidSubscription);
  const auto result = internal::VehicleTelemetryAccess::service(*telemetry_)
                          .unsubscribe_generic(decode(subscription));
  if (!result.ok())
    return SignalStatusResult::failure(map_status(result.status));
  return SignalStatusResult::success();
}

} // namespace mazda
