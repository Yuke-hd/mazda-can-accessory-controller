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
  if (control_kind(*kind) && payload_size > kMaximumControlPayload)
    return FrameHeader{};
  return FrameHeader{*kind, payload_size, 2 + length_bytes + kMaskBytes};
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
  if (kind == ClientFrameKind::Close) {
    finished_ = true;
    buffer_.clear();
  }
  return frame;
}

} // namespace replay
