#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>

#include "can_bus/can_bus.h"
#include "can_bus/configuration.hpp"
#include "can_bus/frame_ring.hpp"

namespace {

vehicle_core::RawCanFrame make_frame(const std::uint32_t identifier,
                                     const vehicle_core::MonotonicTimestamp timestamp) {
  vehicle_core::RawCanFrame frame{};
  frame.timestamp_us = timestamp;
  frame.bus_id = 2;
  frame.identifier = identifier;
  frame.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  frame.remote_request = true;
  frame.dlc = 8;
  frame.data = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  return frame;
}

} // namespace

TEST_CASE("configuration permits only explicit classic CAN bitrates") {
  CHECK(can_bus::internal::is_configuration_valid({125'000, 0}));
  CHECK(can_bus::internal::is_configuration_valid({250'000, 0}));
  CHECK(can_bus::internal::is_configuration_valid({500'000, 0}));
  CHECK(can_bus::internal::is_configuration_valid({1'000'000, 0}));
  CHECK_FALSE(can_bus::internal::is_configuration_valid({0, 0}));
  CHECK_FALSE(can_bus::internal::is_configuration_valid({100'000, 0}));
  CHECK_FALSE(can_bus::internal::is_configuration_valid({499'999, 0}));
}

TEST_CASE("driver counter deltas handle monotonic values and uint32 wraparound") {
  CHECK(can_bus::internal::counter_delta(10, 4) == 6);
  CHECK(can_bus::internal::counter_delta(2, std::numeric_limits<std::uint32_t>::max() - 1) == 4);
  CHECK(can_bus::internal::counter_delta(0, 0) == 0);
}

TEST_CASE("frame boundary preserves every receive field") {
  can_bus::internal::FrameRing<4> ring;
  const auto input = make_frame(0x1abcdef0, 123'456);
  REQUIRE(ring.push(input));

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.timestamp_us == input.timestamp_us);
  CHECK(output.bus_id == input.bus_id);
  CHECK(output.identifier == input.identifier);
  CHECK(output.identifier_format == input.identifier_format);
  CHECK(output.remote_request == input.remote_request);
  CHECK(output.dlc == input.dlc);
  CHECK(output.data == input.data);
}

TEST_CASE("frame boundary distinguishes standard data and extended RTR frames") {
  can_bus::internal::FrameRing<4> ring;
  auto standard = make_frame(0x321, 10);
  standard.identifier_format = vehicle_core::CanIdentifierFormat::Standard;
  standard.remote_request = false;
  standard.dlc = 3;
  standard.data = {0xa5, 0x5a, 0xc3, 0, 0, 0, 0, 0};
  const auto extended_rtr = make_frame(0x1abcde0, 20);
  REQUIRE(ring.push(standard));
  REQUIRE(ring.push(extended_rtr));

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 0x321);
  CHECK_FALSE(output.is_extended());
  CHECK_FALSE(output.remote_request);
  CHECK(output.dlc == 3);
  CHECK(output.data[0] == 0xa5);
  CHECK(output.data[1] == 0x5a);
  CHECK(output.data[2] == 0xc3);

  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 0x1abcde0);
  CHECK(output.is_extended());
  CHECK(output.remote_request);
  CHECK(output.dlc == 8);
}

TEST_CASE("frame boundary is FIFO") {
  can_bus::internal::FrameRing<3> ring;
  REQUIRE(ring.push(make_frame(1, 10)));
  REQUIRE(ring.push(make_frame(2, 20)));
  REQUIRE(ring.push(make_frame(3, 30)));

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 1);
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 2);
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 3);
  CHECK_FALSE(ring.pop(output));
}

TEST_CASE("full boundary drops newest without disturbing queued frames") {
  can_bus::internal::FrameRing<2> ring;
  REQUIRE(ring.push(make_frame(1, 10)));
  REQUIRE(ring.push(make_frame(2, 20)));
  CHECK_FALSE(ring.push(make_frame(3, 30)));

  const auto stats = ring.snapshot(can_bus::StatisticsOperation::kSnapshot);
  CHECK(stats.frames_received == 3);
  CHECK(stats.frames_queued == 2);
  CHECK(stats.frames_dropped == 1);
  CHECK(stats.queue_overflows == 1);
  CHECK(stats.queue_depth == 2);
  CHECK(stats.queue_high_watermark == 2);
  CHECK(stats.queue_capacity == 2);

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 1);
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 2);
}

TEST_CASE("absent and slow consumers do not prevent later producer calls") {
  can_bus::internal::FrameRing<4> ring;
  for (std::uint32_t id = 0; id < 1000; ++id) {
    (void)ring.push(make_frame(id, id));
  }
  const auto full = ring.snapshot(can_bus::StatisticsOperation::kSnapshot);
  CHECK(full.frames_received == 1000);
  CHECK(full.frames_queued == 4);
  CHECK(full.frames_dropped == 996);

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 0);
  REQUIRE(ring.push(make_frame(1000, 1000)));
  CHECK(ring.snapshot(can_bus::StatisticsOperation::kSnapshot).queue_depth == 4);
}

TEST_CASE("statistics reset preserves queue and starts watermark at current depth") {
  can_bus::internal::FrameRing<4> ring;
  REQUIRE(ring.push(make_frame(1, 10)));
  REQUIRE(ring.push(make_frame(2, 20)));
  ring.record_bus_error(3);
  ring.record_driver_rx_missed(4);
  ring.record_controller_reset();
  ring.record_bus_off(5);

  const auto before = ring.snapshot(can_bus::StatisticsOperation::kSnapshotAndReset);
  CHECK(before.frames_received == 2);
  CHECK(before.bus_errors == 3);
  CHECK(before.driver_rx_missed == 4);
  CHECK(before.controller_resets == 1);
  CHECK(before.bus_off_events == 5);
  CHECK(before.queue_depth == 2);

  const auto reset = ring.snapshot(can_bus::StatisticsOperation::kSnapshot);
  CHECK(reset.frames_received == 0);
  CHECK(reset.frames_queued == 0);
  CHECK(reset.frames_delivered == 0);
  CHECK(reset.frames_dropped == 0);
  CHECK(reset.bus_errors == 0);
  CHECK(reset.driver_rx_missed == 0);
  CHECK(reset.controller_resets == 0);
  CHECK(reset.bus_off_events == 0);
  CHECK(reset.queue_depth == 2);
  CHECK(reset.queue_high_watermark == 2);

  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 1);
  REQUIRE(ring.pop(output));
  CHECK(output.identifier == 2);
}

TEST_CASE("statistics reset keeps the next watermark at the live queue depth") {
  can_bus::internal::FrameRing<4> ring;
  REQUIRE(ring.push(make_frame(1, 10)));
  REQUIRE(ring.push(make_frame(2, 20)));
  REQUIRE(ring.push(make_frame(3, 30)));
  vehicle_core::RawCanFrame output{};
  REQUIRE(ring.pop(output));
  REQUIRE(ring.pop(output));

  const auto before = ring.snapshot(can_bus::StatisticsOperation::kSnapshotAndReset);
  CHECK(before.queue_depth == 1);
  CHECK(before.queue_high_watermark == 3);
  CHECK(ring.snapshot(can_bus::StatisticsOperation::kSnapshot).queue_high_watermark == 1);
}

TEST_CASE("statistics retries a forced consumer interleaving before subtracting indexes") {
  struct ForcedInterleaving {
    std::array<can_bus::internal::RingIndex, 6> indexes{
        can_bus::internal::RingIndex::kTail, can_bus::internal::RingIndex::kHead,
        can_bus::internal::RingIndex::kTail, can_bus::internal::RingIndex::kTail,
        can_bus::internal::RingIndex::kHead, can_bus::internal::RingIndex::kTail};
    std::array<std::size_t, 6> values{3, 3, 4, 4, 6, 4};
    std::size_t calls{0};
    bool order_is_coherent{true};

    std::size_t operator()(const can_bus::internal::RingIndex index) noexcept {
      if (calls >= indexes.size() || indexes[calls] != index) {
        order_is_coherent = false;
        return 0;
      }
      return values[calls++];
    }
  } interleaving;

  const auto depth = can_bus::internal::coherent_depth<4>(interleaving);
  CHECK(interleaving.order_is_coherent);
  CHECK(interleaving.calls == 6);
  CHECK(depth == 2);
}

TEST_CASE("statistics retries an out-of-capacity index distance") {
  struct OutOfCapacitySample {
    std::array<std::size_t, 6> values{0, 9, 0, 2, 4, 2};
    std::size_t calls{0};

    std::size_t operator()(const can_bus::internal::RingIndex) noexcept { return values[calls++]; }
  } sample;

  CHECK(can_bus::internal::coherent_depth<4>(sample) == 2);
  CHECK(sample.calls == 6);
}

TEST_CASE("concurrent producer consumer and reset snapshots keep metrics bounded") {
  constexpr std::size_t kCapacity = 4;
  constexpr std::uint32_t kFramesToProduce = 200'000;
  can_bus::internal::FrameRing<kCapacity> ring;
  std::atomic<bool> start{false};
  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};

  auto check_snapshot = [&](const can_bus::Statistics &stats) {
    if (stats.queue_capacity != kCapacity || stats.queue_depth > kCapacity ||
        stats.queue_high_watermark > kCapacity) {
      failed.store(true, std::memory_order_relaxed);
    }
  };

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::uint32_t id = 0; id < kFramesToProduce; ++id) {
      (void)ring.push(make_frame(id, id));
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    vehicle_core::RawCanFrame frame{};
    for (;;) {
      if (ring.pop(frame)) {
        continue;
      }
      if (producer_done.load(std::memory_order_acquire)) {
        break;
      }
      std::this_thread::yield();
    }
  });

  std::thread observer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    constexpr std::size_t kMinimumObservationsAfterProducer = 20'000;
    std::size_t observations_after_producer = 0;
    while (!producer_done.load(std::memory_order_acquire) ||
           observations_after_producer < kMinimumObservationsAfterProducer) {
      const auto operation = observations_after_producer % 11 == 0
                                 ? can_bus::StatisticsOperation::kSnapshotAndReset
                                 : can_bus::StatisticsOperation::kSnapshot;
      check_snapshot(ring.snapshot(operation));
      if (producer_done.load(std::memory_order_acquire)) {
        ++observations_after_producer;
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  observer.join();

  vehicle_core::RawCanFrame frame{};
  while (ring.pop(frame)) {
  }
  check_snapshot(ring.snapshot(can_bus::StatisticsOperation::kSnapshot));
  CHECK_FALSE(failed.load(std::memory_order_relaxed));
}

TEST_CASE("acquisition boundary has fixed storage and value semantics") {
  CHECK(std::is_trivially_copyable_v<vehicle_core::RawCanFrame>);
  CHECK(can_bus::kQueueCapacity == 64);
  CHECK(sizeof(can_bus::internal::FrameRing<4>) < 512);
}
