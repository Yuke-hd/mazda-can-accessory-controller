#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/extensible_live_stream.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace companion_protocol;
using namespace vehicle_signals;
using std::chrono::milliseconds;

class NumberProvider final : public SignalProvider {
public:
  explicit NumberProvider(const std::size_t signal_count, const bool duplicate_key = false) noexcept
      : signal_count_(signal_count) {
    for (std::size_t index = 0; index < signal_count_; ++index) {
      keys_[index] = "test.number." + std::to_string(index + 1U);
      if (duplicate_key && index == 1U) {
        keys_[index] = keys_[0];
      }
      entries_[index] = SignalMetadata{SignalId{static_cast<std::uint16_t>(index + 1U)},
                                       keys_[index],
                                       SignalType::Number,
                                       SignalUnit::None,
                                       ValidationStatus::Confirmed,
                                       SignalCapability::Read,
                                       nullptr,
                                       0};
      readings_[index] = SignalReading{SignalValue::number(0.0F), Availability::Fresh,
                                       ValidationStatus::Confirmed};
    }
  }

  [[nodiscard]] SignalCatalogView catalog() const noexcept override {
    return SignalCatalogView{entries_.data(), signal_count_};
  }

  [[nodiscard]] SignalResult<SignalReading> read(const SignalId id) const noexcept override {
    if (!id.valid() || id.value() > signal_count_) {
      return SignalResult<SignalReading>::failure(SignalStatus::InvalidSignal);
    }
    const std::size_t index = id.value() - 1U;
    ++read_counts_[index];
    return SignalResult<SignalReading>::success(readings_[index]);
  }

  [[nodiscard]] SignalResult<SignalSubscription> subscribe(SignalId, SignalCallback,
                                                           void *) noexcept override {
    return SignalResult<SignalSubscription>::failure(SignalStatus::UnsupportedCapability);
  }

  [[nodiscard]] SignalStatusResult unsubscribe(SignalSubscription) noexcept override {
    return SignalStatusResult::failure(SignalStatus::InvalidSubscription);
  }

  void set_number(const std::uint16_t handle, const float value) noexcept {
    if (handle != 0 && handle <= signal_count_) {
      readings_[handle - 1U] = SignalReading{SignalValue::number(value), Availability::Fresh,
                                             ValidationStatus::Confirmed};
    }
  }

  void set_all(const float value) noexcept {
    for (std::uint16_t handle = 1; handle <= signal_count_; ++handle) {
      set_number(handle, value);
    }
  }

  void reset_read_counts() const noexcept { read_counts_.fill(0); }

  [[nodiscard]] std::size_t read_count(const std::uint16_t handle) const noexcept {
    return handle != 0 && handle <= signal_count_ ? read_counts_[handle - 1U] : 0;
  }

private:
  std::array<std::string, kMaxSignalCatalogSignals> keys_{};
  std::array<SignalMetadata, kMaxSignalCatalogSignals> entries_{};
  std::array<SignalReading, kMaxSignalCatalogSignals> readings_{};
  mutable std::array<std::size_t, kMaxSignalCatalogSignals> read_counts_{};
  std::size_t signal_count_{0};
};

class RecordingSink final : public ExtensibleLiveFrameSink {
public:
  bool try_queue(const ExtensibleLiveSignalPacket &packet) noexcept override {
    packets.push_back(packet);
    return accept;
  }

  bool accept{true};
  std::vector<ExtensibleLiveSignalPacket> packets{};
};

struct Fixture {
  explicit Fixture(const std::size_t signal_count = 1, const bool duplicate_key = false)
      : provider(signal_count, duplicate_key), catalog(provider.catalog()),
        stream(catalog, provider) {}

  NumberProvider provider;
  SignalCatalogExport catalog;
  ExtensibleLiveSignalStream stream;
  RecordingSink sink;
};

std::uint16_t packet_u16(const ExtensibleLiveSignalPacket &packet, const std::size_t offset) {
  return static_cast<std::uint16_t>(packet[offset]) |
         static_cast<std::uint16_t>(packet[offset + 1U] << 8U);
}

std::uint32_t packet_u32(const ExtensibleLiveSignalPacket &packet, const std::size_t offset) {
  return static_cast<std::uint32_t>(packet[offset]) |
         (static_cast<std::uint32_t>(packet[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(packet[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(packet[offset + 3U]) << 24U);
}

std::vector<std::uint16_t> packet_handles(const ExtensibleLiveSignalPacket &packet) {
  std::vector<std::uint16_t> handles;
  std::size_t offset = kExtensibleLiveSignalHeaderBytes;
  while (offset < packet.size()) {
    REQUIRE(offset + 1U <= packet.size());
    const std::size_t record_bytes = 1U + packet[offset];
    REQUIRE(record_bytes >= kExtensibleLiveSignalRecordHeaderBytes);
    REQUIRE(offset + record_bytes <= packet.size());
    handles.push_back(packet_u16(packet, offset + 1U));
    offset += record_bytes;
  }
  return handles;
}

float first_number(const ExtensibleLiveSignalPacket &packet) {
  REQUIRE(packet.size() >= kExtensibleLiveSignalHeaderBytes + 9U);
  std::uint32_t bits = packet_u32(packet, kExtensibleLiveSignalHeaderBytes + 5U);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void make_streamable(ExtensibleLiveSignalStream &stream, const std::uint16_t mtu = 64,
                     const std::uint32_t token = 0x12345678U) {
  stream.set_notifications_enabled(true);
  stream.set_link_secured(true);
  stream.set_att_mtu(mtu);
  stream.set_catalog_token(token);
}

TEST_CASE("extensible stream requires every gate and caps sampling at ten hertz") {
  Fixture fixture;
  fixture.provider.set_number(1, 1.0F);

  CHECK_FALSE(fixture.stream.streaming());
  CHECK_FALSE(fixture.stream.offer(milliseconds{0}, true, fixture.sink));
  fixture.stream.set_notifications_enabled(true);
  fixture.stream.set_link_secured(true);
  fixture.stream.set_catalog_token(0x01020304U);
  fixture.stream.set_att_mtu(23);
  CHECK_FALSE(fixture.stream.streaming());
  fixture.stream.set_att_mtu(63);
  CHECK_FALSE(fixture.stream.streaming());
  CHECK_FALSE(fixture.stream.offer(milliseconds{0}, true, fixture.sink));

  fixture.stream.set_att_mtu(64);
  REQUIRE(fixture.stream.streaming());
  REQUIRE(fixture.stream.time_until_sample(milliseconds{0}) == milliseconds{0});
  REQUIRE(fixture.stream.offer(milliseconds{0}, true, fixture.sink));
  REQUIRE(fixture.sink.packets.size() == 1);
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 0);
  CHECK(fixture.sink.packets.back()[3] == 1);
  CHECK(packet_u32(fixture.sink.packets.back(), 4) == 0x01020304U);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(1.0F));

  fixture.provider.set_number(1, 2.0F);
  CHECK_FALSE(fixture.stream.offer(milliseconds{99}, true, fixture.sink));
  REQUIRE(fixture.stream.time_until_sample(milliseconds{99}) == milliseconds{1});
  REQUIRE(fixture.stream.offer(milliseconds{100}, true, fixture.sink));
  REQUIRE(fixture.sink.packets.size() == 2);
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 1);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(2.0F));
}

TEST_CASE("an unusable catalog keeps the extensible stream disabled") {
  Fixture fixture{2, true};
  make_streamable(fixture.stream, 247);
  CHECK_FALSE(fixture.catalog.available());
  CHECK_FALSE(fixture.stream.streaming());
  CHECK_FALSE(fixture.stream.time_until_sample(milliseconds{0}).has_value());
  CHECK_FALSE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));
  CHECK(fixture.sink.packets.empty());
}

TEST_CASE(
    "accepted records heartbeat at one second and each signal is read at most once per slot") {
  Fixture fixture{3};
  make_streamable(fixture.stream, 247);
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));

  for (int slot = 1; slot < 10; ++slot) {
    fixture.provider.reset_read_counts();
    CHECK_FALSE(fixture.stream.offer(milliseconds{slot * 100}, false, fixture.sink));
    for (std::uint16_t handle = 1; handle <= 3; ++handle) {
      CHECK(fixture.provider.read_count(handle) == 1);
    }
  }
  REQUIRE(fixture.stream.offer(milliseconds{1000}, false, fixture.sink));
  REQUIRE(fixture.sink.packets.size() == 2);
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 1);
  CHECK(packet_handles(fixture.sink.packets.back()).size() == 3);
}

TEST_CASE("queue refusal keeps records due and retries the latest value without history") {
  Fixture fixture;
  make_streamable(fixture.stream);
  fixture.provider.set_number(1, 1.0F);
  fixture.sink.accept = false;
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(1.0F));

  fixture.provider.set_number(1, 7.0F);
  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 1);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(7.0F));

  fixture.sink.accept = true;
  REQUIRE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 2);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(7.0F));
  CHECK_FALSE(fixture.stream.offer(milliseconds{300}, false, fixture.sink));
}

TEST_CASE("partial packet acceptance updates only the records that were queued") {
  Fixture fixture{6};
  make_streamable(fixture.stream, 64);

  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));
  const auto first_handles = packet_handles(fixture.sink.packets.back());
  REQUIRE(first_handles.size() == 5);
  CHECK(first_handles.front() == 1);
  CHECK(first_handles.back() == 5);

  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  const auto second_handles = packet_handles(fixture.sink.packets.back());
  REQUIRE(second_handles.size() == 1);
  CHECK(second_handles.front() == 6);
}

TEST_CASE("refusal after acceptance preserves the accepted record and heartbeat time") {
  Fixture fixture;
  make_streamable(fixture.stream);
  fixture.provider.set_number(1, 1.0F);
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));

  fixture.provider.set_number(1, 2.0F);
  fixture.sink.accept = false;
  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(2.0F));

  fixture.provider.set_number(1, 1.0F);
  fixture.sink.accept = true;
  CHECK_FALSE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  for (int slot = 3; slot < 10; ++slot) {
    CHECK_FALSE(fixture.stream.offer(milliseconds{slot * 100}, false, fixture.sink));
  }
  REQUIRE(fixture.stream.offer(milliseconds{1000}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 2);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(1.0F));
}

void check_full_coverage(const std::uint16_t mtu, const bool accept,
                         const std::size_t expected_attempts) {
  Fixture fixture{64};
  make_streamable(fixture.stream, mtu);
  fixture.sink.accept = accept;
  std::array<bool, 64> seen{};

  for (std::size_t slot = 0; slot < expected_attempts; ++slot) {
    fixture.provider.set_all(static_cast<float>(slot + 1U));
    fixture.provider.reset_read_counts();
    REQUIRE(fixture.stream.offer(milliseconds{static_cast<std::int64_t>(slot * 100U)}, false,
                                 fixture.sink));
    for (const std::uint16_t handle : packet_handles(fixture.sink.packets.back())) {
      REQUIRE(handle >= 1);
      REQUIRE(handle <= 64);
      seen[handle - 1U] = true;
    }
    for (std::uint16_t handle = 1; handle <= 64; ++handle) {
      CHECK(fixture.provider.read_count(handle) <= 1);
    }
  }

  CHECK(fixture.sink.packets.size() == expected_attempts);
  for (const bool covered : seen) {
    CHECK(covered);
  }
}

TEST_CASE("fair rotation covers 64 continuously changing Numbers within 1.3 seconds") {
  SUBCASE("minimum MTU with successful queueing") { check_full_coverage(64, true, 13); }
  SUBCASE("minimum MTU despite repeated queue refusal") { check_full_coverage(64, false, 13); }
  SUBCASE("MTU 247") { check_full_coverage(247, true, 3); }
}

TEST_CASE("all 64 unchanged Numbers receive heartbeat attempts within the coverage deadline") {
  Fixture fixture{64};
  make_streamable(fixture.stream, 64);
  fixture.provider.set_all(42.0F);
  std::array<std::optional<milliseconds>, 64> first_attempt{};
  std::array<std::optional<milliseconds>, 64> heartbeat_attempt{};

  for (std::int64_t slot = 0; slot <= 26; ++slot) {
    const milliseconds now{slot * 100};
    if (!fixture.stream.offer(now, false, fixture.sink)) {
      continue;
    }
    for (const std::uint16_t handle : packet_handles(fixture.sink.packets.back())) {
      auto &first = first_attempt[handle - 1U];
      auto &heartbeat = heartbeat_attempt[handle - 1U];
      if (!first.has_value()) {
        first = now;
      } else if (!heartbeat.has_value()) {
        heartbeat = now;
      }
    }
  }

  for (std::size_t index = 0; index < first_attempt.size(); ++index) {
    REQUIRE(first_attempt[index].has_value());
    REQUIRE(heartbeat_attempt[index].has_value());
    const auto heartbeat_elapsed = *heartbeat_attempt[index] - *first_attempt[index];
    CHECK(heartbeat_elapsed >= kLiveHeartbeatInterval);
    CHECK(heartbeat_elapsed - kLiveHeartbeatInterval <= milliseconds{1300});
  }
}

TEST_CASE("CCCD and accepted-bond restarts clear baselines but preserve the rate cap") {
  Fixture fixture;
  make_streamable(fixture.stream);
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));

  fixture.stream.set_notifications_enabled(false);
  fixture.stream.set_notifications_enabled(true);
  CHECK_FALSE(fixture.stream.offer(milliseconds{99}, false, fixture.sink));
  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 0);

  fixture.stream.set_link_secured(false);
  CHECK_FALSE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  fixture.stream.set_link_secured(true);
  REQUIRE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 0);
}

TEST_CASE("token changes and disconnect restart sequence and accepted map state") {
  Fixture fixture;
  make_streamable(fixture.stream, 64, 0x11111111U);
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));

  fixture.stream.set_catalog_token(0x22222222U);
  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 0);
  CHECK(packet_u32(fixture.sink.packets.back(), 4) == 0x22222222U);

  fixture.stream.reset_connection();
  CHECK_FALSE(fixture.stream.streaming());
  CHECK_FALSE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  make_streamable(fixture.stream, 64, 0x33333333U);
  REQUIRE(fixture.stream.offer(milliseconds{200}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 0);
  CHECK(packet_u32(fixture.sink.packets.back(), 4) == 0x33333333U);
}

TEST_CASE("config pause suppresses reads and resumes without restarting the stream") {
  Fixture fixture;
  make_streamable(fixture.stream);
  REQUIRE(fixture.stream.offer(milliseconds{0}, false, fixture.sink));
  fixture.provider.set_number(1, 8.0F);
  fixture.provider.reset_read_counts();

  fixture.stream.set_config_paused(true);
  CHECK(fixture.stream.streaming());
  CHECK_FALSE(fixture.stream.time_until_sample(milliseconds{100}).has_value());
  CHECK_FALSE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(fixture.provider.read_count(1) == 0);

  fixture.stream.set_config_paused(false);
  REQUIRE(fixture.stream.offer(milliseconds{100}, false, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 1);
  CHECK(first_number(fixture.sink.packets.back()) == doctest::Approx(8.0F));
}

TEST_CASE("an empty usable catalog sends startup and one-second heartbeat packets") {
  Fixture fixture{0};
  make_streamable(fixture.stream);
  REQUIRE(fixture.catalog.available());
  REQUIRE(fixture.stream.offer(milliseconds{0}, true, fixture.sink));
  REQUIRE(fixture.sink.packets.back().size() == kExtensibleLiveSignalHeaderBytes);
  CHECK_FALSE(fixture.stream.offer(milliseconds{100}, true, fixture.sink));
  REQUIRE(fixture.stream.offer(milliseconds{1000}, true, fixture.sink));
  CHECK(packet_u16(fixture.sink.packets.back(), 1) == 1);

  Fixture refused{0};
  make_streamable(refused.stream);
  refused.sink.accept = false;
  REQUIRE(refused.stream.offer(milliseconds{0}, false, refused.sink));
  REQUIRE(refused.stream.offer(milliseconds{100}, false, refused.sink));
  CHECK(packet_u16(refused.sink.packets.back(), 1) == 1);
}

TEST_CASE("the wrapping sequence counts notification attempts") {
  Fixture fixture;
  make_streamable(fixture.stream);
  bool every_attempt_was_offered = true;
  std::array<std::uint16_t, 3> boundary_sequences{};
  for (std::uint32_t attempt = 0; attempt <= 0x10000U; ++attempt) {
    fixture.provider.set_number(1, static_cast<float>(attempt));
    every_attempt_was_offered &= fixture.stream.offer(
        milliseconds{static_cast<std::int64_t>(attempt) * 100}, false, fixture.sink);
    every_attempt_was_offered &= fixture.sink.packets.size() == 1;
    if (!fixture.sink.packets.empty() &&
        (attempt == 0 || attempt == 0xFFFFU || attempt == 0x10000U)) {
      const std::size_t boundary = attempt == 0 ? 0 : (attempt == 0xFFFFU ? 1 : 2);
      boundary_sequences[boundary] = packet_u16(fixture.sink.packets.back(), 1);
    }
    fixture.sink.packets.clear();
  }
  CHECK(every_attempt_was_offered);
  CHECK(boundary_sequences[0] == 0);
  CHECK(boundary_sequences[1] == 0xFFFFU);
  CHECK(boundary_sequences[2] == 0);
}

} // namespace
