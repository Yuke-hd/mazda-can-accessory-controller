#include "mazda/signal_provider.hpp"

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_subscription_bridge.hpp"
#include "mazda/signal_value_conversion.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <atomic>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace mazda {

namespace {

using vehicle_signals::SignalId;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalStatusResult;
using vehicle_signals::SignalSubscription;

// Typed channel callback. `record` is the trampoline record registered as the
// channel context; it stays unchanged while the channel can dispatch to it.
template <typename T> void trampoline(void *record, const Notification<T> &notice) noexcept {
  const auto &subscription = *static_cast<const internal::SignalSubscriptionRecord *>(record);
  subscription.callback(subscription.context,
                        internal::to_signal_notification(subscription.id, notice));
}

// Generic token layout (the typed SubscriptionToken fields, which keep the
// channel generation and therefore stale-token protection):
//   bits  0..15 generation (never zero for a registration)
//   bits 16..23 slot
//   bits 32..47 channel
// Every other bit is zero, so a successful token is never the invalid zero.
// Unsubscribe decodes by exact comparison against the live records' typed
// tokens, so any other bit pattern is rejected.
constexpr unsigned kSlotShift = 16U;
constexpr unsigned kChannelShift = 32U;

[[nodiscard]] constexpr SignalSubscription
encode(const internal::SubscriptionToken &token) noexcept {
  return SignalSubscription::from_provider_bits((std::uint64_t{token.channel} << kChannelShift) |
                                                (std::uint64_t{token.slot} << kSlotShift) |
                                                std::uint64_t{token.generation});
}

[[nodiscard]] constexpr bool matches(const internal::SubscriptionToken &token,
                                     const SignalSubscription subscription) noexcept {
  return encode(token) == subscription;
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

// Scoped try-lock over the bridge records. A contended mutation is by
// definition not the only lifecycle-owner mutation and is rejected.
class MutationGuard final {
public:
  explicit MutationGuard(std::atomic<bool> &mutating) noexcept
      : mutating_(mutating), acquired_(!mutating.exchange(true, std::memory_order_acquire)) {}
  ~MutationGuard() noexcept {
    if (acquired_)
      mutating_.store(false, std::memory_order_release);
  }
  MutationGuard(const MutationGuard &) = delete;
  MutationGuard &operator=(const MutationGuard &) = delete;

  [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
  std::atomic<bool> &mutating_;
  bool acquired_;
};

// Registers `record` on the production typed channel bound to its SignalId.
// A Notify id without a descriptor keeps the default InvalidState token; the
// catalog/descriptor binding tests make that unreachable.
[[nodiscard]] internal::SubscriptionToken
register_record(internal::VehicleTelemetryService &service,
                internal::SignalSubscriptionRecord &record) noexcept {
  internal::SubscriptionToken token{};
  bool found = false;
  std::apply(
      [&](const auto &...descriptor) {
        (([&] {
           using Descriptor = std::decay_t<decltype(descriptor)>;
           using Value = typename Descriptor::Channel::Value;
           if (found || !descriptor.id.valid() || descriptor.id != record.id)
             return;
           found = true;
           token =
               service.subscribe_notification_descriptor(descriptor, &trampoline<Value>, &record);
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

  auto &bridge = internal::subscription_bridge(subscription_storage_);
  const MutationGuard guard{bridge.mutating};
  if (!guard.acquired())
    return Result::failure(SignalStatus::InvalidState);

  internal::SignalSubscriptionRecord *record = nullptr;
  for (auto &candidate : bridge.records) {
    if (!candidate.active) {
      record = &candidate;
      break;
    }
  }
  // Every record holds one of the shared typed slots, so no free record means
  // every Notify channel is already full.
  if (record == nullptr)
    return Result::failure(SignalStatus::CapacityExceeded);

  // Fill the record before registering: the channel may dispatch to it as
  // soon as the next start() after a successful registration.
  record->id = id;
  record->callback = callback;
  record->context = context;
  record->token = {};
  auto &service = internal::VehicleTelemetryAccess::service(*telemetry_);
  const auto token = register_record(service, *record);
  if (!token.ok()) {
    *record = {};
    return Result::failure(map_status(token.status));
  }
  record->token = token;
  record->active = true;
  return Result::success(encode(token));
}

vehicle_signals::SignalStatusResult
MazdaSignalProvider::unsubscribe(const SignalSubscription subscription) noexcept {
  // Only this provider's own live records are candidates. A stale, forged or
  // foreign token never reaches the service, so it cannot remove a typed
  // subscriber that shares the channel.
  if (!subscription.valid())
    return SignalStatusResult::failure(SignalStatus::InvalidSubscription);

  auto &bridge = internal::subscription_bridge(subscription_storage_);
  const MutationGuard guard{bridge.mutating};
  if (!guard.acquired())
    return SignalStatusResult::failure(SignalStatus::InvalidState);

  for (auto &record : bridge.records) {
    if (!record.active || !matches(record.token, subscription))
      continue;
    const auto result =
        internal::VehicleTelemetryAccess::service(*telemetry_).unsubscribe(record.token);
    if (!result.ok())
      return SignalStatusResult::failure(map_status(result.status));
    record = {};
    return SignalStatusResult::success();
  }
  return SignalStatusResult::failure(SignalStatus::InvalidSubscription);
}

// Precondition (header): the facade is stopped and this runs on its lifecycle
// owner, so the service accepts every unsubscribe and no dispatcher can be
// inside a trampoline. Each record is cleared whatever the result, because
// the provider storage is about to end.
MazdaSignalProvider::~MazdaSignalProvider() noexcept {
  auto &bridge = internal::subscription_bridge(subscription_storage_);
  auto &service = internal::VehicleTelemetryAccess::service(*telemetry_);
  for (auto &record : bridge.records) {
    if (record.active)
      (void)service.unsubscribe(record.token);
    record = {};
  }
}

} // namespace mazda
