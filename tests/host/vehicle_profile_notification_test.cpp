#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>

#include "provider_access.hpp"
#include "vehicle_profile_runtime/provider.hpp"
#include "vehicle_profile_runtime/tables.hpp"

namespace allocation_probe {
std::atomic<bool> enabled{false};
std::atomic<std::size_t> allocations{0};

void record() noexcept {
  if (enabled.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
  }
}
} // namespace allocation_probe

void *operator new(const std::size_t size) {
  allocation_probe::record();
  if (void *const memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
  allocation_probe::record();
  if (void *const memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

void operator delete(void *const memory) noexcept { std::free(memory); }
void operator delete(void *const memory, const std::size_t) noexcept { std::free(memory); }
void operator delete[](void *const memory) noexcept { std::free(memory); }
void operator delete[](void *const memory, const std::size_t) noexcept { std::free(memory); }

namespace {

using vehicle_profile_runtime::DecodeResult;
using vehicle_profile_runtime::DispatchEntry;
using vehicle_profile_runtime::ProfileProvider;
using vehicle_profile_runtime::ProfileTables;
using vehicle_profile_runtime::ProviderAccess;
using vehicle_profile_runtime::PublicationSynchronization;
using vehicle_profile_runtime::SignalFreshnessPolicy;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    now_us_.store(now_us, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

class FakeSynchronization final {
public:
  [[nodiscard]] PublicationSynchronization binding() noexcept {
    return PublicationSynchronization{this, &enter, &leave, this, &claim_owner};
  }

  [[nodiscard]] bool available_to_callback() noexcept {
    if (!mutex_.try_lock()) {
      return false;
    }
    mutex_.unlock();
    return true;
  }

private:
  static void enter(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.lock();
  }

  static void leave(void *context) noexcept {
    static_cast<FakeSynchronization *>(context)->mutex_.unlock();
  }

  static bool claim_owner(void *context) noexcept {
    auto &self = *static_cast<FakeSynchronization *>(context);
    std::lock_guard<std::mutex> lock{self.owner_mutex_};
    const std::thread::id current = std::this_thread::get_id();
    if (!self.owner_claimed_) {
      self.owner_ = current;
      self.owner_claimed_ = true;
      return true;
    }
    return self.owner_ == current;
  }

  std::mutex mutex_{};
  std::mutex owner_mutex_{};
  std::thread::id owner_{};
  bool owner_claimed_{false};
};

constexpr SignalMetadata kSignals[]{
    {SignalId{1}, "notify_primary", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Confirmed, SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
    {SignalId{2}, "notify_secondary", SignalType::Boolean, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Read | SignalCapability::Notify, nullptr, 0},
    {SignalId{3}, "read_only", SignalType::Boolean, SignalUnit::None, ValidationStatus::Reference,
     SignalCapability::Read, nullptr, 0},
};
constexpr SignalFreshnessPolicy kFreshness[]{
    {SignalId{1}, true, 10},
    {SignalId{2}, false, 0},
    {SignalId{3}, false, 0},
};
constexpr SignalId kOutputs[]{SignalId{1}};

void decode(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
            const std::size_t capacity, std::size_t &count) {
  (void)vehicle_profile_runtime::append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{1}, SignalValue::boolean(frame.data[0] != 0U)));
}

constexpr DispatchEntry kDispatch[]{{0x101U, 1U, &decode, kOutputs, std::size(kOutputs)}};

const ProfileTables kTables{SignalCatalogView{kSignals}, kFreshness,
                            std::size(kFreshness),       kDispatch,
                            std::size(kDispatch),        std::size(kOutputs)};

vehicle_core::RawCanFrame frame(const vehicle_core::MonotonicTimestamp timestamp_us,
                                const std::uint8_t value) noexcept {
  vehicle_core::RawCanFrame result{};
  result.identifier = 0x101U;
  result.timestamp_us = timestamp_us;
  result.dlc = 1U;
  result.data[0] = value;
  return result;
}

struct NotificationLog final {
  std::array<SignalNotification, 8> notices{};
  std::size_t count{0};
  FakeSynchronization *synchronization{nullptr};
  bool callback_lock_free{true};
};

void capture(void *context, const SignalNotification &notice) noexcept {
  auto &log = *static_cast<NotificationLog *>(context);
  if (log.synchronization != nullptr && !log.synchronization->available_to_callback()) {
    log.callback_lock_free = false;
  }
  if (log.count < log.notices.size()) {
    log.notices[log.count++] = notice;
  }
}

struct MutationContext final {
  ProfileProvider *provider{nullptr};
  vehicle_signals::SignalSubscription handle{};
  SignalStatus subscribe_status{SignalStatus::Ok};
  SignalStatus unsubscribe_status{SignalStatus::Ok};
  bool callback_active{false};
};

void attempt_mutation(void *context, const SignalNotification &) noexcept {
  auto &mutation = *static_cast<MutationContext *>(context);
  mutation.callback_active = ProviderAccess::callback_active(*mutation.provider);
  const auto subscribed = mutation.provider->subscribe(SignalId{1}, &capture, nullptr);
  mutation.subscribe_status = subscribed.status;
  mutation.unsubscribe_status = mutation.provider->unsubscribe(mutation.handle).status;
}

TEST_CASE("Notify registrations use two shared bounded slots and preserve replacements") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  NotificationLog first{};
  NotificationLog second{};
  NotificationLog replacement{};
  NotificationLog other_channel_log{};
  const auto first_handle = provider.subscribe(SignalId{1}, &capture, &first);
  REQUIRE(first_handle.ok());
  const auto second_handle = provider.subscribe(SignalId{1}, &capture, &second);
  REQUIRE(second_handle.ok());
  CHECK(provider.subscribe(SignalId{1}, &capture, nullptr).status ==
        SignalStatus::CapacityExceeded);
  const auto other_channel = provider.subscribe(SignalId{2}, &capture, &other_channel_log);
  REQUIRE(other_channel.ok());

  CHECK(provider.unsubscribe(*first_handle.value).ok());
  const auto replacement_handle = provider.subscribe(SignalId{1}, &capture, &replacement);
  REQUIRE(replacement_handle.ok());
  CHECK(provider.unsubscribe(*first_handle.value).status == SignalStatus::InvalidSubscription);

  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  CHECK(first.count == 0U);
  CHECK(second.count == 1U);
  CHECK(replacement.count == 1U);
  CHECK(replacement.notices[0].initial);
  CHECK(replacement.notices[0].current.validation == ValidationStatus::Confirmed);
  CHECK(provider.unsubscribe(*replacement_handle.value).status == SignalStatus::InvalidState);
  ProviderAccess::set_running(provider, false);

  CHECK(provider.unsubscribe(*second_handle.value).status == SignalStatus::InvalidSubscription);
  CHECK(provider.unsubscribe(*replacement_handle.value).status ==
        SignalStatus::InvalidSubscription);
  CHECK(provider.unsubscribe(*other_channel.value).status == SignalStatus::InvalidSubscription);
}

TEST_CASE("Notify callbacks receive copied readings outside publication synchronization") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  NotificationLog log{};
  log.synchronization = &synchronization;
  const auto handle = provider.subscribe(SignalId{1}, &capture, &log);
  REQUIRE(handle.ok());

  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  clock.set(1U);
  const auto first = ProviderAccess::process(provider, frame(1U, 1U));
  ProviderAccess::deliver(provider, first);
  REQUIRE(log.count == 2U);
  REQUIRE(log.notices[1].current.value.has_value());
  CHECK(log.notices[1].current.value->as_boolean() == true);
  CHECK(log.callback_lock_free);

  clock.set(2U);
  const auto second = ProviderAccess::process(provider, frame(2U, 0U));
  ProviderAccess::deliver(provider, second);
  REQUIRE(log.count == 3U);
  CHECK(log.notices[1].current.value->as_boolean() == true);
  CHECK(log.notices[2].current.value->as_boolean() == false);
  ProviderAccess::set_running(provider, false);
}

TEST_CASE("Notify mutations from callbacks fail while the provider runs") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  MutationContext mutation{};
  mutation.provider = &provider;
  const auto handle = provider.subscribe(SignalId{1}, &attempt_mutation, &mutation);
  REQUIRE(handle.ok());
  mutation.handle = *handle.value;

  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  CHECK(mutation.callback_active);
  CHECK(mutation.subscribe_status == SignalStatus::InvalidState);
  CHECK(mutation.unsubscribe_status == SignalStatus::InvalidState);
  ProviderAccess::set_running(provider, false);
  CHECK(provider.unsubscribe(*handle.value).status == SignalStatus::InvalidSubscription);
}

TEST_CASE("due then recovery before dispatch preserves unavailable and coalesced flags") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  NotificationLog log{};
  const auto handle = provider.subscribe(SignalId{1}, &capture, &log);
  REQUIRE(handle.ok());

  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  clock.set(1U);
  const auto initial_value = ProviderAccess::process(provider, frame(1U, 1U));
  ProviderAccess::deliver(provider, initial_value);
  REQUIRE(log.count == 2U);

  clock.set(10002U);
  const auto due_then_recovered = ProviderAccess::process(provider, frame(10002U, 0U));
  REQUIRE(due_then_recovered.due_count == 1U);
  REQUIRE(due_then_recovered.recovered_count == 1U);
  ProviderAccess::deliver(provider, due_then_recovered);
  REQUIRE(log.count == 3U);
  CHECK(log.notices[2].current.availability == Availability::Fresh);
  CHECK(log.notices[2].became_unavailable);
  CHECK(log.notices[2].recovered);
  CHECK(log.notices[2].coalesced);
  ProviderAccess::set_running(provider, false);
}

TEST_CASE("successful stop ends callback context borrows and clears handles") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  NotificationLog first_context{};
  NotificationLog next_context{};
  const auto old_handle = provider.subscribe(SignalId{1}, &capture, &first_context);
  REQUIRE(old_handle.ok());

  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  REQUIRE(first_context.count == 1U);
  ProviderAccess::set_running(provider, false);
  CHECK(provider.unsubscribe(*old_handle.value).status == SignalStatus::InvalidSubscription);

  const auto new_handle = provider.subscribe(SignalId{1}, &capture, &next_context);
  REQUIRE(new_handle.ok());
  ProviderAccess::set_running(provider, true);
  ProviderAccess::deliver(provider, {});
  CHECK(first_context.count == 1U);
  CHECK(next_context.count == 1U);
  ProviderAccess::set_running(provider, false);
}

TEST_CASE("Notify mutations stay on the first lifecycle owner across threads") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};
  NotificationLog log{};
  const auto handle = provider.subscribe(SignalId{1}, &capture, &log);
  REQUIRE(handle.ok());

  SignalStatus other_thread_subscribe{SignalStatus::Ok};
  SignalStatus other_thread_unsubscribe{SignalStatus::Ok};
  std::thread other_thread([&] {
    other_thread_subscribe = provider.subscribe(SignalId{2}, &capture, &log).status;
    other_thread_unsubscribe = provider.unsubscribe(*handle.value).status;
  });
  other_thread.join();

  CHECK(other_thread_subscribe == SignalStatus::InvalidState);
  CHECK(other_thread_unsubscribe == SignalStatus::InvalidState);
  CHECK(provider.unsubscribe(*handle.value).ok());
}

TEST_CASE("Notify mutation fails closed without an injected lifecycle owner") {
  FakeClock clock;
  FakeSynchronization synchronization;
  auto binding = synchronization.binding();
  binding.lifecycle_owner_context = nullptr;
  binding.claim_or_validate_lifecycle_owner = nullptr;
  ProfileProvider provider{kTables, clock, binding};
  NotificationLog log{};

  CHECK(provider.read(SignalId{1}).ok());
  CHECK(provider.subscribe(SignalId{1}, &capture, &log).status == SignalStatus::InvalidState);
}

TEST_CASE("Notify registration enforces the 17-binding generated limit") {
  constexpr std::array<std::string_view, 18> keys{
      "notify_01", "notify_02", "notify_03", "notify_04", "notify_05", "notify_06",
      "notify_07", "notify_08", "notify_09", "notify_10", "notify_11", "notify_12",
      "notify_13", "notify_14", "notify_15", "notify_16", "notify_17", "notify_18"};
  std::array<SignalMetadata, 18> signals{};
  std::array<SignalFreshnessPolicy, 18> freshness{};
  for (std::size_t index = 0; index < signals.size(); ++index) {
    const auto id = SignalId{static_cast<std::uint16_t>(index + 1U)};
    signals[index] = SignalMetadata{id,
                                    keys[index],
                                    SignalType::Boolean,
                                    SignalUnit::None,
                                    ValidationStatus::Reference,
                                    SignalCapability::Notify,
                                    nullptr,
                                    0};
    freshness[index] = SignalFreshnessPolicy{id, false, 0};
  }

  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileTables at_limit{
      SignalCatalogView{signals.data(), 17U}, freshness.data(), 17U, nullptr, 0U, 0U};
  ProfileProvider supported{at_limit, clock, synchronization.binding()};
  NotificationLog log{};
  CHECK(supported.subscribe(SignalId{17}, &capture, &log).ok());

  ProfileTables over_limit{SignalCatalogView{signals.data(), signals.size()},
                           freshness.data(),
                           freshness.size(),
                           nullptr,
                           0U,
                           0U};
  ProfileProvider rejected{over_limit, clock, synchronization.binding()};
  CHECK(rejected.subscribe(SignalId{1}, &capture, &log).status == SignalStatus::Faulted);
}

TEST_CASE("Notify provider construction and runtime operations allocate no heap") {
  allocation_probe::allocations.store(0, std::memory_order_relaxed);
  bool registered = false;
  bool unsubscribed = false;
  bool read_ok = false;
  allocation_probe::enabled.store(true, std::memory_order_relaxed);
  {
    FakeClock clock;
    FakeSynchronization synchronization;
    ProfileProvider provider{kTables, clock, synchronization.binding()};
    NotificationLog log{};
    const auto registration = provider.subscribe(SignalId{1}, &capture, &log);
    registered = registration.ok();
    if (registration.ok()) {
      unsubscribed = provider.unsubscribe(*registration.value).ok();
    }
    const auto active_registration = provider.subscribe(SignalId{1}, &capture, &log);
    registered = registered && active_registration.ok();
    ProviderAccess::set_running(provider, true);
    ProviderAccess::deliver(provider, {});
    clock.set(1U);
    const auto frame_result = ProviderAccess::process(provider, frame(1U, 1U));
    ProviderAccess::deliver(provider, frame_result);
    read_ok = provider.read(SignalId{1}).ok();
    const auto due_result = ProviderAccess::service(provider);
    ProviderAccess::deliver(provider, due_result);
    const auto fault_result = ProviderAccess::fault(provider);
    ProviderAccess::deliver(provider, fault_result);
    const auto reset_result = ProviderAccess::reset(provider);
    ProviderAccess::deliver(provider, reset_result);
    ProviderAccess::set_running(provider, false);
  }
  allocation_probe::enabled.store(false, std::memory_order_relaxed);
  CHECK(registered);
  CHECK(unsubscribed);
  CHECK(read_ok);
  CHECK(allocation_probe::allocations.load(std::memory_order_relaxed) == 0U);
}

TEST_CASE("Notify subscription validates capability and null callback before registration") {
  FakeClock clock;
  FakeSynchronization synchronization;
  ProfileProvider provider{kTables, clock, synchronization.binding()};

  CHECK(provider.subscribe(SignalId{99}, &capture, nullptr).status == SignalStatus::InvalidSignal);
  CHECK(provider.subscribe(SignalId{3}, &capture, nullptr).status ==
        SignalStatus::UnsupportedCapability);
  CHECK(provider.subscribe(SignalId{1}, nullptr, nullptr).status == SignalStatus::InvalidArgument);
}

} // namespace
