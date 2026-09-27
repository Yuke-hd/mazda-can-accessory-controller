#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>

#include "replay/clock.hpp"

TEST_CASE("replay clock starts at zero by default") {
  const replay::ReplayClock clock;

  CHECK(clock.now() == 0);
}

TEST_CASE("replay clock starts at a caller-defined timestamp") {
  const replay::ReplayClock clock{42'000};

  CHECK(clock.now() == 42'000);
}

TEST_CASE("replay clock advances directly to a later timestamp") {
  replay::ReplayClock clock{100};

  CHECK(clock.advance_to(900));
  CHECK(clock.now() == 900);
}

TEST_CASE("replay clock accepts an equal timestamp") {
  replay::ReplayClock clock{700};

  CHECK(clock.advance_to(700));
  CHECK(clock.now() == 700);
}

TEST_CASE("replay clock rejects a timestamp regression without changing time") {
  replay::ReplayClock clock{800};

  CHECK_FALSE(clock.advance_to(799));
  CHECK(clock.now() == 800);
}

TEST_CASE("replay clock supports the largest monotonic timestamp") {
  replay::ReplayClock clock;
  constexpr auto largest_timestamp = std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();

  CHECK(clock.advance_to(largest_timestamp));
  CHECK(clock.now() == largest_timestamp);
}
