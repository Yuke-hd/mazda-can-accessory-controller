#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/display_link.hpp"
#include "support/fake_signal_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

using namespace companion_protocol;

PairingRecord sample_record() {
  PairingRecord record{};
  record.peer = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  record.channel = 6;
  for (std::size_t index = 0; index < record.lmk.size(); ++index) {
    record.lmk[index] = static_cast<std::uint8_t>(0xA0 + index);
  }
  return record;
}

std::uint32_t draws[4]{};
std::size_t draw_index = 0;
std::uint32_t scripted_random() noexcept { return draws[draw_index++]; }

std::string_view code_text(const SetupCode &code) { return {code.data(), code.size()}; }

} // namespace

TEST_CASE("pairing record round-trips through its fixed byte layout") {
  const PairingRecord record = sample_record();
  const PairingRecordBytes bytes = encode_pairing_record(record);
  CHECK(bytes[0] == kPairingRecordVersion);
  CHECK(bytes[1] == 0x02);
  CHECK(bytes[7] == 6);
  CHECK(bytes[8] == 1); // pairing profile v1
  CHECK(bytes[9] == 1); // ECAN v1
  CHECK(bytes[10] == 0xA0);
  const auto decoded = decode_pairing_record(bytes.data(), bytes.size());
  REQUIRE(decoded.has_value());
  CHECK(decoded->peer == record.peer);
  CHECK(decoded->channel == record.channel);
  CHECK(decoded->lmk == record.lmk);
}

TEST_CASE("pairing record decode rejects every malformed field") {
  const PairingRecordBytes good = encode_pairing_record(sample_record());
  CHECK_FALSE(decode_pairing_record(good.data(), good.size() - 1).has_value());
  CHECK_FALSE(decode_pairing_record(nullptr, good.size()).has_value());
  const auto rejects = [&](std::size_t offset, std::uint8_t value) {
    PairingRecordBytes bytes = good;
    bytes[offset] = value;
    return !decode_pairing_record(bytes.data(), bytes.size()).has_value();
  };
  CHECK(rejects(0, 2));    // unknown record version
  CHECK(rejects(1, 0x03)); // multicast MAC (bit 0 of first octet)
  CHECK(rejects(7, 0));    // channel below 1
  CHECK(rejects(7, 14));   // channel above 13
  CHECK(rejects(8, 2));    // unknown pairing profile
  CHECK(rejects(9, 2));    // unknown ECAN version

  PairingRecord zero_mac = sample_record();
  zero_mac.peer = {};
  const auto zero_mac_bytes = encode_pairing_record(zero_mac);
  CHECK_FALSE(decode_pairing_record(zero_mac_bytes.data(), zero_mac_bytes.size()).has_value());

  PairingRecord zero_lmk = sample_record();
  zero_lmk.lmk = {};
  const auto zero_lmk_bytes = encode_pairing_record(zero_lmk);
  CHECK_FALSE(decode_pairing_record(zero_lmk_bytes.data(), zero_lmk_bytes.size()).has_value());
}

TEST_CASE("setup code normalization") {
  const auto plain = normalize_setup_code("0123456789ABCDEF");
  REQUIRE(plain.has_value());
  CHECK(code_text(*plain) == "0123456789ABCDEF");
  const auto spaced = normalize_setup_code("01 23-45 67-89ab cdef");
  REQUIRE(spaced.has_value());
  CHECK(code_text(*spaced) == "0123456789ABCDEF");
  CHECK_FALSE(normalize_setup_code("0123456789ABCDE").has_value());
  CHECK_FALSE(normalize_setup_code("0123456789ABCDEF0").has_value());
  CHECK_FALSE(normalize_setup_code("0123456789ABCDEG").has_value());
  CHECK_FALSE(normalize_setup_code("").has_value());
  CHECK_FALSE(normalize_setup_code("0123456789ABCDE\n").has_value());
}

TEST_CASE("long press fires once after five seconds of a fresh press") {
  LongPressDetector detector;
  for (int index = 0; index < 10; ++index) {
    CHECK_FALSE(detector.sample(UserKeyLevel::Released));
  }
  int fired = 0;
  int fired_at = -1;
  for (int index = 0; index < 400; ++index) {
    if (detector.sample(UserKeyLevel::Pressed)) {
      ++fired;
      fired_at = index;
    }
  }
  CHECK(fired == 1);
  CHECK(fired_at == static_cast<int>(kLongPressSamples) - 1);
}

TEST_CASE("long press needs an unbroken hold and ignores a key held since boot") {
  LongPressDetector boot_hold;
  for (std::size_t index = 0; index < 2 * kLongPressSamples; ++index) {
    CHECK_FALSE(boot_hold.sample(UserKeyLevel::Pressed));
  }

  LongPressDetector broken;
  for (int index = 0; index < 5; ++index) {
    (void)broken.sample(UserKeyLevel::Released);
  }
  for (std::size_t index = 0; index < kLongPressSamples - 10; ++index) {
    CHECK_FALSE(broken.sample(UserKeyLevel::Pressed));
  }
  CHECK_FALSE(broken.sample(UserKeyLevel::Released));
  for (int index = 0; index < 20; ++index) {
    CHECK_FALSE(broken.sample(UserKeyLevel::Pressed));
  }
}

TEST_CASE("publisher draws a nonzero session and wraps its sequence") {
  vehicle_signals::SignalCatalogView empty{};
  test_support::FakeSignalProvider provider{empty};
  draws[0] = 0;
  draws[1] = 0x01020304U;
  draw_index = 0;
  SnapshotPublisher publisher{provider, scripted_random, 0xfffffffeU};
  CHECK(publisher.session() == 0x01020304U);

  const auto first = publisher.next();
  CHECK(first[5] == 0x00); // not started yet: stopped packet
  CHECK(first[8] == 0x04);
  CHECK(first[12] == 0xfe);
  publisher.mark_telemetry_started();
  const auto second = publisher.next();
  CHECK(second[5] == 0x01);
  CHECK(second[12] == 0xff);
  CHECK(second[15] == 0xff);
  const auto third = publisher.next();
  CHECK(third[12] == 0x00);
  CHECK(third[15] == 0x00);
}
