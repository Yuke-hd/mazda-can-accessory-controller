#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>

#include "can_bus/can_bus.h"
#include "can_bus/frame_ring.hpp"
#include "can_bus/lifecycle.hpp"

namespace {

// The host adapter models only driver outcomes. It deliberately does not
// duplicate TWAI or FreeRTOS; the lifecycle controller is the portable seam
// exercised by the fault matrix, while can_bus.cpp owns the real adapter.
class FaultInjectionAdapter final {
public:
  explicit FaultInjectionAdapter(can_bus::internal::LifecycleController &lifecycle)
      : lifecycle_(lifecycle) {}

  void install() noexcept { lifecycle_.mark_driver_installed(); }
  [[nodiscard]] bool start(const bool succeeds) noexcept {
    if (!succeeds) {
      return false;
    }
    lifecycle_.mark_driver_started();
    return true;
  }
  void create_task() noexcept { lifecycle_.mark_task_created(); }

  // A timeout is intentionally a no-op. The receive task must continue its
  // bounded polling cadence instead of treating idle traffic as a fault.
  void receive_idle_timeout() noexcept {}

  // The adapter reports whether this is the first terminal fault. Production
  // code uses that edge to wake the single consumer exactly once.
  [[nodiscard]] bool receive_failure(bool &consumer_wakeup) noexcept {
    if (!lifecycle_.latch_fault()) {
      return false;
    }
    consumer_wakeup = true;
    return true;
  }

  [[nodiscard]] bool status_failure(bool &consumer_wakeup) noexcept {
    return receive_failure(consumer_wakeup);
  }

  [[nodiscard]] bool bus_off(can_bus::internal::FrameRing<4> &metrics,
                             bool &consumer_wakeup) noexcept {
    metrics.record_bus_off();
    return receive_failure(consumer_wakeup);
  }

  [[nodiscard]] bool stop(const bool succeeds) noexcept {
    if (!succeeds) {
      return false;
    }
    lifecycle_.mark_driver_stopped();
    return true;
  }

  [[nodiscard]] bool uninstall(const bool succeeds) noexcept {
    if (!succeeds) {
      return false;
    }
    lifecycle_.mark_driver_uninstalled();
    return true;
  }

  void acknowledge_task() noexcept { lifecycle_.acknowledge_task(); }

private:
  can_bus::internal::LifecycleController &lifecycle_;
};

void complete_cleanup(can_bus::internal::LifecycleController &lifecycle,
                      FaultInjectionAdapter &adapter) {
  CHECK(adapter.stop(true));
  adapter.acknowledge_task();
  CHECK(adapter.uninstall(true));
  lifecycle.reconcile();
}

} // namespace

TEST_CASE("terminal receive failure latches and wakes the consumer once") {
  can_bus::internal::LifecycleController lifecycle;
  FaultInjectionAdapter adapter(lifecycle);
  REQUIRE(lifecycle.begin_start());
  adapter.install();
  REQUIRE(adapter.start(true));
  adapter.create_task();

  bool consumer_wakeup = false;
  CHECK(adapter.receive_failure(consumer_wakeup));
  CHECK(consumer_wakeup);
  CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);
  CHECK(lifecycle.task_owned());
  CHECK(lifecycle.driver_installed());

  // A second status/receive failure does not create an unbounded wakeup or
  // retry loop after the first terminal edge.
  consumer_wakeup = false;
  CHECK_FALSE(adapter.receive_failure(consumer_wakeup));
  CHECK_FALSE(consumer_wakeup);
}

TEST_CASE("idle receive timeout is nonfatal and bounded polling can continue") {
  can_bus::internal::LifecycleController lifecycle;
  FaultInjectionAdapter adapter(lifecycle);
  REQUIRE(lifecycle.begin_start());
  adapter.install();
  REQUIRE(adapter.start(true));
  adapter.create_task();

  for (std::size_t i = 0; i < 100; ++i) {
    adapter.receive_idle_timeout();
  }
  CHECK(lifecycle.state() == can_bus::LifecycleState::kRunning);
  CHECK(lifecycle.task_owned());
}

TEST_CASE("delayed task acknowledgement retains stopping ownership for retry") {
  can_bus::internal::LifecycleController lifecycle;
  FaultInjectionAdapter adapter(lifecycle);
  REQUIRE(lifecycle.begin_start());
  adapter.install();
  REQUIRE(adapter.start(true));
  adapter.create_task();

  REQUIRE(lifecycle.begin_stop());
  CHECK(lifecycle.state() == can_bus::LifecycleState::kStopping);
  CHECK(adapter.stop(true));
  lifecycle.reconcile();
  CHECK(lifecycle.state() == can_bus::LifecycleState::kStopping);
  CHECK(lifecycle.task_owned());

  // A timed-out cleanup attempt does not release ownership. A later attempt
  // may finish after the task acknowledgement without a second driver.
  REQUIRE(lifecycle.begin_stop());
  adapter.acknowledge_task();
  CHECK(adapter.uninstall(true));
  lifecycle.reconcile();
  CHECK(lifecycle.state() == can_bus::LifecycleState::kStopped);
  CHECK(lifecycle.begin_start());
}

TEST_CASE("failed stop and uninstall remain faulted until a later cleanup") {
  SUBCASE("stop failure keeps the driver owner") {
    can_bus::internal::LifecycleController lifecycle;
    FaultInjectionAdapter adapter(lifecycle);
    REQUIRE(lifecycle.begin_start());
    adapter.install();
    REQUIRE(adapter.start(true));
    adapter.create_task();
    REQUIRE(lifecycle.begin_stop());
    CHECK_FALSE(adapter.stop(false));
    adapter.acknowledge_task();
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);
    CHECK(lifecycle.driver_started());
    CHECK_FALSE(lifecycle.begin_start());

    CHECK(adapter.stop(true));
    CHECK(adapter.uninstall(true));
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kStopped);
  }

  SUBCASE("uninstall failure keeps the stopped driver installed") {
    can_bus::internal::LifecycleController lifecycle;
    FaultInjectionAdapter adapter(lifecycle);
    REQUIRE(lifecycle.begin_start());
    adapter.install();
    REQUIRE(adapter.start(true));
    adapter.create_task();
    REQUIRE(lifecycle.begin_stop());
    CHECK(adapter.stop(true));
    adapter.acknowledge_task();
    CHECK_FALSE(adapter.uninstall(false));
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);
    CHECK(lifecycle.driver_installed());

    CHECK(adapter.uninstall(true));
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kStopped);
  }
}

TEST_CASE("startup failure can be retried and never races an owned run") {
  can_bus::internal::LifecycleController lifecycle;
  FaultInjectionAdapter adapter(lifecycle);
  REQUIRE(lifecycle.begin_start());
  CHECK_FALSE(lifecycle.begin_start());
  lifecycle.abort_start();
  CHECK(lifecycle.state() == can_bus::LifecycleState::kStopped);

  REQUIRE(lifecycle.begin_start());
  adapter.install();
  REQUIRE(adapter.start(true));
  adapter.create_task();
  CHECK_FALSE(lifecycle.begin_start());
  CHECK(lifecycle.begin_stop());
  CHECK_FALSE(lifecycle.begin_start());
  complete_cleanup(lifecycle, adapter);
  REQUIRE(lifecycle.state() == can_bus::LifecycleState::kStopped);
  CHECK(lifecycle.begin_start());
}

TEST_CASE("status and start faults are terminal but cleanup remains retryable") {
  SUBCASE("status failure wakes the consumer") {
    can_bus::internal::LifecycleController lifecycle;
    FaultInjectionAdapter adapter(lifecycle);
    REQUIRE(lifecycle.begin_start());
    adapter.install();
    REQUIRE(adapter.start(true));
    adapter.create_task();

    bool consumer_wakeup = false;
    CHECK(adapter.status_failure(consumer_wakeup));
    CHECK(consumer_wakeup);
    CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);
  }

  SUBCASE("failed driver start retains an installed owner") {
    can_bus::internal::LifecycleController lifecycle;
    FaultInjectionAdapter adapter(lifecycle);
    REQUIRE(lifecycle.begin_start());
    adapter.install();
    CHECK_FALSE(adapter.start(false));
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);
    CHECK_FALSE(lifecycle.begin_start());

    CHECK(adapter.uninstall(true));
    lifecycle.reconcile();
    CHECK(lifecycle.state() == can_bus::LifecycleState::kStopped);
  }
}

TEST_CASE("bus-off faults use a separate metric from controller resets") {
  can_bus::internal::LifecycleController lifecycle;
  can_bus::internal::FrameRing<4> metrics;
  FaultInjectionAdapter adapter(lifecycle);
  REQUIRE(lifecycle.begin_start());
  adapter.install();
  REQUIRE(adapter.start(true));
  adapter.create_task();

  bool consumer_wakeup = false;
  CHECK(adapter.bus_off(metrics, consumer_wakeup));
  CHECK(consumer_wakeup);
  CHECK(lifecycle.state() == can_bus::LifecycleState::kFaulted);

  const auto stats = metrics.snapshot(can_bus::StatisticsOperation::kSnapshotAndReset);
  CHECK(stats.bus_off_events == 1);
  CHECK(stats.controller_resets == 0);
}
