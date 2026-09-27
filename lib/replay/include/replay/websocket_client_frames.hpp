#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace replay {

// What a browser sent on the emulator WebSocket. Anything the emulator does
// not accept ends the connection with the matching close code.
enum class ClientFrameKind : std::uint8_t {
  Text,          // A complete, unmasked text payload.
  Ping,          // Must be answered with a pong carrying the same payload.
  Pong,          // Ignored.
  Close,         // The browser is closing; answer with close 1000.
  ProtocolError, // Unmasked, fragmented, reserved bits, or bad control frame: close 1002.
  Unsupported,   // Binary data: close 1003.
  TooLarge,      // A text message longer than the control limit: close 1009.
};

struct ClientFrame final {
  ClientFrameKind kind{ClientFrameKind::ProtocolError};
  std::string payload{};
};

// Close code the emulator sends in reply to a frame kind, or 0 when the frame
// does not end the connection.
[[nodiscard]] std::uint16_t close_code_for(ClientFrameKind kind) noexcept;

// Incrementally decodes client-to-server WebSocket frames (RFC 6455 5.2).
// Browser control messages are tiny, so the decoder buffers at most one frame
// of up to max_text_bytes and reports longer frames as TooLarge as soon as
// their header arrives, without waiting for the payload.
class ClientFrameDecoder final {
public:
  explicit ClientFrameDecoder(std::size_t max_text_bytes) noexcept
      : max_text_bytes_(max_text_bytes) {}

  void append(std::string_view bytes);

  // The next complete frame, or nullopt when more bytes are needed. After a
  // frame that ends the connection, the decoder yields nothing more.
  [[nodiscard]] std::optional<ClientFrame> next();

private:
  std::size_t max_text_bytes_;
  std::string buffer_;
  bool finished_{false};
};

} // namespace replay
