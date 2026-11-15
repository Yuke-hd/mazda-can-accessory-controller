#include "companion_protocol/display_link.hpp"

#include <algorithm>

namespace companion_protocol {

namespace {

constexpr std::uint8_t kPairingProfileVersion = 1;
constexpr std::uint8_t kEcanVersion = 1;
constexpr std::size_t kMacOffset = 1;
constexpr std::size_t kChannelOffset = 7;
constexpr std::size_t kProfileOffset = 8;
constexpr std::size_t kEcanOffset = 9;
constexpr std::size_t kLmkOffset = 10;
static_assert(kLmkOffset + 16 == kPairingRecordBytes, "record layout");

template <std::size_t N> bool all_zero(const std::array<std::uint8_t, N> &bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; });
}

} // namespace

PairingRecordBytes encode_pairing_record(const PairingRecord &record) noexcept {
  PairingRecordBytes bytes{};
  bytes[0] = kPairingRecordVersion;
  std::copy(record.peer.begin(), record.peer.end(), bytes.begin() + kMacOffset);
  bytes[kChannelOffset] = record.channel;
  bytes[kProfileOffset] = kPairingProfileVersion;
  bytes[kEcanOffset] = kEcanVersion;
  std::copy(record.lmk.begin(), record.lmk.end(), bytes.begin() + kLmkOffset);
  return bytes;
}

std::optional<PairingRecord> decode_pairing_record(const std::uint8_t *bytes,
                                                   std::size_t size) noexcept {
  if (bytes == nullptr || size != kPairingRecordBytes || bytes[0] != kPairingRecordVersion ||
      bytes[kProfileOffset] != kPairingProfileVersion || bytes[kEcanOffset] != kEcanVersion) {
    return std::nullopt;
  }
  PairingRecord record{};
  std::copy(bytes + kMacOffset, bytes + kChannelOffset, record.peer.begin());
  record.channel = bytes[kChannelOffset];
  std::copy(bytes + kLmkOffset, bytes + kPairingRecordBytes, record.lmk.begin());
  if (all_zero(record.peer) || (record.peer[0] & 0x01U) != 0 || record.channel < 1 ||
      record.channel > 13 || all_zero(record.lmk)) {
    return std::nullopt;
  }
  return record;
}

std::optional<SetupCode> normalize_setup_code(std::string_view text) noexcept {
  SetupCode code{};
  std::size_t count = 0;
  for (const char raw : text) {
    if (raw == ' ' || raw == '-') {
      continue;
    }
    const char upper = (raw >= 'a' && raw <= 'f') ? static_cast<char>(raw - 'a' + 'A') : raw;
    const bool hex = (upper >= '0' && upper <= '9') || (upper >= 'A' && upper <= 'F');
    if (!hex || count == kSetupCodeChars) {
      return std::nullopt;
    }
    code[count++] = upper;
  }
  if (count != kSetupCodeChars) {
    return std::nullopt;
  }
  return code;
}

bool LongPressDetector::sample(UserKeyLevel level) noexcept {
  const bool pressed_now = debouncer_.sample(level);
  if (level == UserKeyLevel::Released) {
    armed_ = false;
    held_ = 0;
    return false;
  }
  if (pressed_now) {
    // The debouncer accepts a press after kUserKeyDebounceSamples raw samples.
    armed_ = true;
    held_ = kUserKeyDebounceSamples;
  } else if (armed_) {
    ++held_;
  }
  if (armed_ && held_ >= kLongPressSamples) {
    armed_ = false;
    return true;
  }
  return false;
}

SnapshotPublisher::SnapshotPublisher(const vehicle_signals::SignalProvider &provider,
                                     RandomSource random, std::uint32_t first_sequence) noexcept
    : sampler_(provider), sequence_(first_sequence) {
  while (session_ == 0) {
    session_ = random();
  }
}

DisplaySnapshot SnapshotPublisher::next() noexcept {
  return sampler_.sample(started_.load(std::memory_order_relaxed), session_, sequence_++);
}

} // namespace companion_protocol
