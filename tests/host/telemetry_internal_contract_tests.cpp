#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "local_argb/lighting_sink.hpp"
#include "vehicle_core/decoder_contracts.hpp"

TEST_CASE("decoder and private lighting contracts keep health separate") {
  vehicle_core::DecoderObservation observation{};
  observation.validity = vehicle_core::DecodeValidity::Malformed;
  observation.timestamp_us = 100;
  CHECK(observation.validity == vehicle_core::DecodeValidity::Malformed);

  local_argb::internal::LightingCommand command{};
  command.actionable = false;
  command.valid_until_us = 200;
  CHECK_FALSE(command.actionable);
  CHECK(command.valid_until_us == 200);
}
