#include "replay/websocket_client_frames.hpp"

namespace replay {
namespace {

constexpr std::uint8_t kFinalFragment = 0x80U;
constexpr std::uint8_t kReservedBits = 0x70U;
constexpr std::uint8_t kOpcodeBits = 0x0fU;
constexpr std::uint8_t kMaskBit = 0x80U;
constexpr std::uint8_t kLengthBits = 0x7fU;
constexpr std::size_t kMaskBytes = 4;
constexpr std::uint64_t kMaximumControlPayload = 125;
constexpr std::uint64_t kLengthMostSignificantBit = 1ULL << 63U;
constexpr std::size_t kCloseCodeBytes = 2;

std::uint8_t byte_at(const std::string &buffer, const std::size_t index) noexcept {
  return static_cast<std::uint8_t>(buffer[index]);
}

std::optional<ClientFrameKind> kind_for(const std::uint8_t opcode) noexcept {
  switch (opcode) {
  case 0x1:
    return ClientFrameKind::Text;
  case 0x2:
    return ClientFrameKind::Unsupported;
  case 0x8:
    return ClientFrameKind::Close;
  case 0x9:
    return ClientFrameKind::Ping;
  case 0xA:
    return ClientFrameKind::Pong;
  default:
    return std::nullopt;
  }
}

bool control_kind(const ClientFrameKind kind) noexcept {
  return kind == ClientFrameKind::Close || kind == ClientFrameKind::Ping ||
         kind == ClientFrameKind::Pong;
}

struct FrameHeader final {
  ClientFrameKind kind{ClientFrameKind::ProtocolError};
  std::uint64_t payload_size{0};
  std::size_t size{0}; // Header bytes including the masking key.
};

// Parses the frame header at the front of buffer: nullopt while incomplete.
std::optional<FrameHeader> read_header(const std::string &buffer) {
  if (buffer.size() < 2)
    return std::nullopt;
  const std::uint8_t first = byte_at(buffer, 0);
  const std::uint8_t second = byte_at(buffer, 1);
  const auto kind = kind_for(first & kOpcodeBits);
  if ((first & kFinalFragment) == 0 || (first & kReservedBits) != 0 || (second & kMaskBit) == 0 ||
      !kind)
    return FrameHeader{};

  std::uint64_t payload_size = second & kLengthBits;
  std::size_t length_bytes = 0;
  if (payload_size == 126U)
    length_bytes = 2;
  else if (payload_size == 127U)
    length_bytes = 8;
  if (buffer.size() < 2 + length_bytes)
    return std::nullopt;
  if (length_bytes != 0) {
    payload_size = 0;
    for (std::size_t index = 0; index < length_bytes; ++index)
      payload_size = (payload_size << 8U) | byte_at(buffer, 2 + index);
  }
  // RFC 6455 5.2: the 64-bit length form keeps its most significant bit clear.
  if ((payload_size & kLengthMostSignificantBit) != 0 ||
      (control_kind(*kind) && payload_size > kMaximumControlPayload))
    return FrameHeader{};
  return FrameHeader{*kind, payload_size, 2 + length_bytes + kMaskBytes};
}

// Strict UTF-8 (RFC 3629): no overlong forms, surrogates, or code points
// above U+10FFFF.
bool valid_utf8(const std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<std::uint8_t>(text[index]);
    std::size_t continuation = 0;
    std::uint8_t low = 0x80U;
    std::uint8_t high = 0xBFU;
    if (lead < 0x80U)
      continuation = 0;
    else if (lead >= 0xC2U && lead <= 0xDFU)
      continuation = 1;
    else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation = 2;
      low = lead == 0xE0U ? 0xA0U : 0x80U;
      high = lead == 0xEDU ? 0x9FU : 0xBFU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation = 3;
      low = lead == 0xF0U ? 0x90U : 0x80U;
      high = lead == 0xF4U ? 0x8FU : 0xBFU;
    } else
      return false;
    if (text.size() - index - 1 < continuation)
      return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto next = static_cast<std::uint8_t>(text[index + offset]);
      if (next < low || next > high)
        return false;
      low = 0x80U;
      high = 0xBFU;
    }
    index += continuation + 1;
  }
  return true;
}

// RFC 6455 7.4: status codes an endpoint may send in a close frame.
bool valid_close_status(const std::uint16_t status) noexcept {
  return (status >= 1000 && status <= 1003) || (status >= 1007 && status <= 1014) ||
         (status >= 3000 && status <= 4999);
}

// Checks a complete, unmasked frame's payload: text and close reasons must be
// UTF-8, and a close body is empty or a valid status code plus reason.
ClientFrameKind validated_kind(const ClientFrameKind kind, const std::string_view payload) {
  if (kind == ClientFrameKind::Text)
    return valid_utf8(payload) ? kind : ClientFrameKind::InvalidText;
  if (kind != ClientFrameKind::Close || payload.empty())
    return kind;
  if (payload.size() < kCloseCodeBytes)
    return ClientFrameKind::ProtocolError;
  const auto status = static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8U) |
                                                 static_cast<std::uint8_t>(payload[1]));
  if (!valid_close_status(status))
    return ClientFrameKind::ProtocolError;
  return valid_utf8(payload.substr(kCloseCodeBytes)) ? kind : ClientFrameKind::InvalidText;
}

} // namespace

std::uint16_t close_code_for(const ClientFrameKind kind) noexcept {
  switch (kind) {
  case ClientFrameKind::Close:
    return 1000;
  case ClientFrameKind::ProtocolError:
    return 1002;
  case ClientFrameKind::Unsupported:
    return 1003;
  case ClientFrameKind::InvalidText:
    return 1007;
  case ClientFrameKind::TooLarge:
    return 1009;
  case ClientFrameKind::Text:
  case ClientFrameKind::Ping:
  case ClientFrameKind::Pong:
    break;
  }
  return 0;
}

void ClientFrameDecoder::append(const std::string_view bytes) {
  if (!finished_)
    buffer_.append(bytes.data(), bytes.size());
}

std::optional<ClientFrame> ClientFrameDecoder::next() {
  if (finished_)
    return std::nullopt;
  const auto header = read_header(buffer_);
  if (!header)
    return std::nullopt;
  const ClientFrameKind kind =
      header->kind == ClientFrameKind::Text && header->payload_size > max_text_bytes_
          ? ClientFrameKind::TooLarge
          : header->kind;
  if (close_code_for(kind) > 1000) {
    finished_ = true;
    buffer_.clear();
    return ClientFrame{kind, {}};
  }
  const std::size_t payload_size = static_cast<std::size_t>(header->payload_size);
  if (buffer_.size() < header->size + payload_size)
    return std::nullopt;

  ClientFrame frame{kind, buffer_.substr(header->size, payload_size)};
  const std::size_t mask_offset = header->size - kMaskBytes;
  for (std::size_t index = 0; index < frame.payload.size(); ++index)
    frame.payload[index] = static_cast<char>(byte_at(frame.payload, index) ^
                                             byte_at(buffer_, mask_offset + index % kMaskBytes));
  buffer_.erase(0, header->size + payload_size);
  frame.kind = validated_kind(kind, frame.payload);
  if (close_code_for(frame.kind) > 1000)
    frame.payload.clear();
  if (close_code_for(frame.kind) != 0) {
    finished_ = true;
    buffer_.clear();
  }
  return frame;
}

} // namespace replay
