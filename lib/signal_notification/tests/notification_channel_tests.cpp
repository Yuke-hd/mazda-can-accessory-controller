// Provenance: ported from components/vehicle_core/tests/notification_channel_tests.cpp
// of Yuke-hd/esp32-vehicle-can-core release 0.2.1
// (b3d87ac094576f7a787114a1bd24db5cf427a4f7), Apache-2.0, same owner. The
// cases are unchanged apart from the signal_notification namespace.
#undef NDEBUG
#include "signal_notification/notification_channel.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

using Channel = signal_notification::NotificationChannel<int, 17>;
// This intentionally has no production signal meaning.  It proves a future
// test-only channel uses the same fixed dispatcher without scheduler changes.
using ExtraTestOnlyChannel = signal_notification::NotificationChannel<bool, 18>;
using Status = signal_notification::NotificationStatus;

struct CallbackState {
  Channel *channel{nullptr};
  std::vector<signal_notification::Notification<int>> notices{};
  std::vector<int> order{};
  std::vector<int> *shared_order{nullptr};
  int order_id{0};
  bool publish_during_callback{false};
  std::size_t publishes_remaining{0};
  int next_publish_value{1000};
  Status subscribe_from_callback{Status::Ok};
  Status unsubscribe_from_callback{Status::Ok};
  signal_notification::NotificationHandle handle{};
};

void callback(void *raw, const signal_notification::Notification<int> &notice) noexcept {
  auto &state = *static_cast<CallbackState *>(raw);
  state.notices.push_back(notice);
  state.order.push_back(state.order_id);
  if (state.shared_order != nullptr) {
    state.shared_order->push_back(state.order_id);
  }
  if (state.publish_during_callback) {
    state.publish_during_callback = false;
    vehicle_core::Reading<int> next{};
    next.value = 99;
    next.availability = vehicle_core::Availability::Fresh;
    next.validation = vehicle_core::ValidationStatus::Reference;
    assert(state.channel->publish(next) == Status::Ok);
  }
  if (state.publishes_remaining > 0) {
    --state.publishes_remaining;
    vehicle_core::Reading<int> next{};
    next.value = state.next_publish_value++;
    next.availability = vehicle_core::Availability::Fresh;
    next.validation = vehicle_core::ValidationStatus::Reference;
    assert(state.channel->publish(next) == Status::Ok);
  }
  state.subscribe_from_callback = state.channel->subscribe(&callback, &state).status;
  state.unsubscribe_from_callback = state.channel->unsubscribe(state.handle);
}

struct ConcurrentCallbackState {
  std::atomic<std::size_t> calls{0};
  std::atomic<int> latest{-1};
};

void concurrent_callback(void *raw, const signal_notification::Notification<int> &notice) noexcept {
  auto &state = *static_cast<ConcurrentCallbackState *>(raw);
  ++state.calls;
  if (notice.current.value.has_value()) {
    state.latest.store(*notice.current.value);
  }
}

vehicle_core::Reading<int> available(int value) {
  vehicle_core::Reading<int> reading{};
  reading.value = value;
  reading.availability = vehicle_core::Availability::Fresh;
  reading.validation = vehicle_core::ValidationStatus::Confirmed;
  return reading;
}

vehicle_core::Reading<int> unavailable(int value) {
  vehicle_core::Reading<int> reading = available(value);
  reading.availability = vehicle_core::Availability::Unavailable;
  return reading;
}

void test_initial_latest_and_duplicate_suppression() {
  Channel channel;
  CallbackState state{&channel};
  auto registration = channel.subscribe(&callback, &state);
  assert(registration.ok());
  state.handle = *registration.value;
  assert(channel.subscribe(nullptr, &state).status == Status::InvalidCallback);
  assert(channel.start() == Status::Ok);
  assert(channel.start() == Status::AlreadyRunning);
  assert(channel.publish(available(3)) == Status::Ok);
  assert(channel.publish(available(3)) == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  assert(state.notices.size() == 1);
  assert(state.notices[0].initial);
  assert(!state.notices[0].coalesced);
  assert(state.notices[0].current.value == 3);
  assert(!state.notices[0].became_unavailable);
  assert(!state.notices[0].recovered);
  assert(channel.dispatch_one() == Status::NoPending);
  // The callback was invoked while running, so both mutation routes are
  // rejected.  Reset these values before exercising another callback.
  assert(state.subscribe_from_callback == Status::InvalidState);
  assert(state.unsubscribe_from_callback == Status::InvalidState);
  assert(channel.stop() == Status::Ok);
}

void test_coalesced_failure_and_same_value_recovery() {
  Channel channel;
  CallbackState state{&channel};
  auto registration = channel.subscribe(&callback, &state);
  state.handle = *registration.value;
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok); // initial NoData
  assert(channel.publish(available(7)) == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  assert(channel.publish(unavailable(7)) == Status::Ok);
  assert(channel.publish(available(7)) == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  assert(state.notices.size() == 3);
  const auto &notice = state.notices.back();
  assert(notice.current.value == 7);
  assert(notice.became_unavailable);
  assert(notice.recovered);
  assert(notice.coalesced);
  assert(channel.stop() == Status::Ok);
}

void test_two_slots_capacity_stale_handle_and_registration_order() {
  Channel channel;
  std::vector<int> shared_order{};
  CallbackState first{&channel};
  CallbackState second{&channel};
  CallbackState extra{&channel};
  first.shared_order = &shared_order;
  second.shared_order = &shared_order;
  extra.shared_order = &shared_order;
  first.order_id = 1;
  second.order_id = 2;
  auto first_registration = channel.subscribe(&callback, &first);
  auto second_registration = channel.subscribe(&callback, &second);
  assert(first_registration.ok());
  assert(second_registration.ok());
  first.handle = *first_registration.value;
  second.handle = *second_registration.value;
  assert(channel.subscribe(&callback, &extra).status == Status::CapacityExceeded);
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_pending() == 2);
  assert((shared_order == std::vector<int>{1, 2}));
  assert(first.order == std::vector<int>{1});
  assert(second.order == std::vector<int>{2});
  assert(channel.stop() == Status::Ok);
  assert(channel.unsubscribe(first.handle) == Status::Ok);
  auto replacement = channel.subscribe(&callback, &extra);
  assert(replacement.ok());
  assert(channel.unsubscribe(first.handle) == Status::InvalidSubscription);
  extra.handle = *replacement.value;
  extra.order_id = 3;
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_pending(1) == 1);
  assert((shared_order == std::vector<int>{1, 2, 2}));
  assert(channel.dispatch_pending(1) == 1);
  assert((shared_order == std::vector<int>{1, 2, 2, 3}));
  assert(second.order.back() == 2);
  assert(extra.order.back() == 3);
  assert(channel.stop() == Status::Ok);
}

void test_bounded_latest_state_and_independent_extra_channel() {
  Channel channel;
  CallbackState state{&channel};
  auto registration = channel.subscribe(&callback, &state);
  assert(registration.ok());
  state.handle = *registration.value;
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);

  for (int value = 1; value <= 100'000; ++value) {
    assert(channel.publish(available(value)) == Status::Ok);
  }
  assert(channel.dispatch_one() == Status::Ok);
  assert(state.notices.size() == 2);
  assert(state.notices.back().current.value == 100'000);
  assert(state.notices.back().coalesced);
  assert(channel.dispatch_one() == Status::NoPending);
  assert(channel.stop() == Status::Ok);

  ExtraTestOnlyChannel extra;
  std::size_t extra_count = 0;
  auto extra_registration = extra.subscribe(
      [](void *raw, const signal_notification::Notification<bool> &notice) noexcept {
        auto &count = *static_cast<std::size_t *>(raw);
        if (notice.current.value.has_value()) {
          ++count;
        }
      },
      &extra_count);
  assert(extra_registration.ok());
  assert(extra.start() == Status::Ok);
  vehicle_core::Reading<bool> extra_reading{};
  extra_reading.value = true;
  extra_reading.availability = vehicle_core::Availability::FreshnessUnverified;
  assert(extra.publish(extra_reading) == Status::Ok);
  assert(extra.dispatch_pending() == 1);
  assert(extra_count == 1);
  assert(extra.stop() == Status::Ok);
}

void test_updates_during_callback_survive_detach() {
  Channel channel;
  CallbackState state{&channel};
  auto registration = channel.subscribe(&callback, &state);
  state.handle = *registration.value;
  assert(channel.start() == Status::Ok);
  state.publish_during_callback = true;
  assert(channel.dispatch_one() == Status::Ok);
  assert(state.notices.size() == 1);
  assert(channel.dispatch_one() == Status::Ok);
  assert(state.notices.size() == 2);
  assert(state.notices.back().current.value == 99);
  assert(!state.notices.back().initial);
  assert(channel.stop() == Status::Ok);
}

void test_fair_progress_with_repeated_dispatch_one() {
  Channel channel;
  std::vector<int> shared_order{};
  CallbackState first{&channel};
  CallbackState second{&channel};
  first.shared_order = &shared_order;
  second.shared_order = &shared_order;
  first.order_id = 1;
  second.order_id = 2;
  first.publishes_remaining = 3;
  second.publishes_remaining = 3;

  assert(channel.subscribe(&callback, &first).ok());
  assert(channel.subscribe(&callback, &second).ok());
  assert(channel.start() == Status::Ok);

  for (int pass = 0; pass < 3; ++pass) {
    assert(channel.dispatch_one() == Status::Ok);
    assert(channel.dispatch_one() == Status::Ok);
  }

  assert((shared_order == std::vector<int>{1, 2, 1, 2, 1, 2}));
  assert(first.order.size() == 3);
  assert(second.order.size() == 3);
  assert(channel.stop() == Status::Ok);
}

void test_fair_progress_with_repeated_bounded_dispatch() {
  Channel channel;
  std::vector<int> shared_order{};
  CallbackState first{&channel};
  CallbackState second{&channel};
  first.shared_order = &shared_order;
  second.shared_order = &shared_order;
  first.order_id = 1;
  second.order_id = 2;
  first.publishes_remaining = 4;
  second.publishes_remaining = 4;

  assert(channel.subscribe(&callback, &first).ok());
  assert(channel.subscribe(&callback, &second).ok());
  assert(channel.start() == Status::Ok);

  for (int pass = 0; pass < 8; ++pass) {
    assert(channel.dispatch_pending(1) == 1);
  }

  assert((shared_order == std::vector<int>{1, 2, 1, 2, 1, 2, 1, 2}));
  assert(first.order.size() == 4);
  assert(second.order.size() == 4);
  assert(channel.stop() == Status::Ok);
}

void test_restart_clears_history_and_pending_flags() {
  Channel channel;
  CallbackState state{&channel};
  auto registration = channel.subscribe(&callback, &state);
  state.handle = *registration.value;
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  assert(channel.publish(available(1)) == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  assert(channel.publish(unavailable(1)) == Status::Ok);
  assert(channel.publish(available(1)) == Status::Ok);
  assert(channel.stop() == Status::Ok);
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);
  const auto &notice = state.notices.back();
  assert(notice.initial);
  assert(!notice.became_unavailable);
  assert(!notice.recovered);
  assert(!notice.coalesced);
  assert(!notice.current.value.has_value());
  assert(channel.stop() == Status::Ok);
}

void test_concurrent_publish_and_dispatch_are_race_free() {
  Channel channel;
  ConcurrentCallbackState state;
  auto registration = channel.subscribe(&concurrent_callback, &state);
  assert(registration.ok());
  assert(channel.start() == Status::Ok);
  assert(channel.dispatch_one() == Status::Ok);

  std::atomic<bool> producer_done{false};
  std::thread producer([&]() {
    for (int value = 1; value <= 20'000; ++value) {
      (void)channel.publish(available(value));
    }
    producer_done.store(true);
  });
  std::thread dispatcher([&]() {
    while (!producer_done.load()) {
      (void)channel.dispatch_one();
    }
    while (channel.dispatch_one() == Status::Ok) {
    }
  });
  producer.join();
  dispatcher.join();

  // At least initial state and one published state were delivered; all
  // intermediate values are intentionally allowed to coalesce.
  assert(state.calls.load() >= 2);
  assert(state.latest.load() == 20'000);
  assert(channel.stop() == Status::Ok);
}

} // namespace

int main() {
  test_initial_latest_and_duplicate_suppression();
  test_coalesced_failure_and_same_value_recovery();
  test_two_slots_capacity_stale_handle_and_registration_order();
  test_bounded_latest_state_and_independent_extra_channel();
  test_updates_during_callback_survive_detach();
  test_fair_progress_with_repeated_dispatch_one();
  test_fair_progress_with_repeated_bounded_dispatch();
  test_restart_clears_history_and_pending_flags();
  test_concurrent_publish_and_dispatch_are_race_free();
  return 0;
}
