#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <type_traits>

#include "vehicle_core/frame.hpp"
#include "vehicle_core/signal.hpp"
#include "vehicle_core/vehicle_core.hpp"

namespace {

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp time_us{0};

  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override { return time_us; }
};

} // namespace

TEST_CASE("raw frame construction retains bus and fixed payload") {
  vehicle_core::RawCanFrame frame{};
  frame.timestamp_us = 42;
  frame.bus_id = 3;
  frame.identifier = 0x123;
  frame.dlc = 2;
  frame.data[0] = 0xaa;
  frame.data[1] = 0x55;

  CHECK(frame.is_valid());
  CHECK(frame.bus_id == 3);
  CHECK(frame.timestamp_us == 42);
  CHECK(frame.data[0] == 0xaa);
  CHECK(frame.data[1] == 0x55);
  CHECK(frame.is_extended() == false);
}

TEST_CASE("raw frame validates classic CAN identifier and DLC bounds") {
  vehicle_core::RawCanFrame frame{};
  frame.identifier = 0x800;
  CHECK(frame.is_valid() == false);
  frame.identifier = 0x1fffffff;
  frame.identifier_format = vehicle_core::CanIdentifierFormat::Extended;
  frame.dlc = 8;
  CHECK(frame.is_valid());
  frame.dlc = 9;
  CHECK(frame.is_valid() == false);

  frame.dlc = 8;
  frame.identifier_format = static_cast<vehicle_core::CanIdentifierFormat>(0xff);
  CHECK(frame.is_valid() == false);
}

TEST_CASE("signal status distinguishes valid zero from unknown and stale") {
  vehicle_core::Signal<float> speed{vehicle_core::SignalUnit::KilometresPerHour, 100};
  CHECK(speed.is_unknown());
  CHECK(speed.update(0.0F, 100));
  CHECK(speed.is_valid());
  CHECK(speed.value == 0.0F);

  speed.refresh(200);
  CHECK(speed.is_valid());
  speed.refresh(201);
  CHECK(speed.is_stale());
  CHECK(speed.value == 0.0F);

  vehicle_core::Signal<float> never_updated{vehicle_core::SignalUnit::KilometresPerHour, 1};
  never_updated.refresh(10000);
  CHECK(never_updated.is_unknown());
}

TEST_CASE("signal rejects out of order timestamps") {
  vehicle_core::Signal<int> rpm{vehicle_core::SignalUnit::RevolutionsPerMinute};
  CHECK(rpm.update(1000, 500));
  CHECK(rpm.update(1100, 501));
  CHECK(rpm.update(900, 500) == false);
  CHECK(rpm.value == 1100);
  CHECK(rpm.last_update_us == 501);
  CHECK(rpm.is_valid());
}

TEST_CASE("domain API is plain portable C++") {
  CHECK(std::is_trivially_copyable_v<vehicle_core::RawCanFrame>);
  CHECK(sizeof(vehicle_core::RawCanFrame) <= 32);
  CHECK(vehicle_core::kApiVersion == 1);
  CHECK(vehicle_core::library_is_available());
}
