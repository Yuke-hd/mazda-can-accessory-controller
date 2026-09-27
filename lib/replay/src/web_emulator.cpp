#include "replay/web_emulator.hpp"

#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace replay {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

constexpr std::string_view kWebSocketMagic{"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"};
constexpr std::size_t kMaximumHttpRequestBytes = 16 * 1024;

SocketHandle socket_from_storage(const std::intptr_t value) noexcept {
#if defined(_WIN32)
  return static_cast<SocketHandle>(value);
#else
  return value;
#endif
}

std::intptr_t socket_to_storage(const SocketHandle value) noexcept {
#if defined(_WIN32)
  return static_cast<std::intptr_t>(value);
#else
  return value;
#endif
}

bool valid_socket(const SocketHandle socket) noexcept { return socket != kInvalidSocket; }

void close_socket(const SocketHandle socket) noexcept {
  if (!valid_socket(socket))
    return;
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

void shutdown_socket(const SocketHandle socket) noexcept {
  if (!valid_socket(socket))
    return;
#if defined(_WIN32)
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

bool retryable_socket_error() noexcept {
#if defined(_WIN32)
  const int error = WSAGetLastError();
  return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

void set_receive_timeout(const SocketHandle socket) noexcept {
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 250'000;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
               sizeof(timeout));
}

bool set_nonblocking(const SocketHandle socket) noexcept {
#if defined(_WIN32)
  u_long enabled = 1;
  return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const int flags = ::fcntl(socket, F_GETFL, 0);
  return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool set_blocking(const SocketHandle socket) noexcept {
#if defined(_WIN32)
  u_long disabled = 0;
  return ::ioctlsocket(socket, FIONBIO, &disabled) == 0;
#else
  const int flags = ::fcntl(socket, F_GETFL, 0);
  return flags >= 0 && ::fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

bool send_all(const SocketHandle socket, const std::uint8_t *data,
              const std::size_t size) noexcept {
  std::size_t sent = 0;
  while (sent < size) {
#if defined(_WIN32)
    const int count = ::send(socket, reinterpret_cast<const char *>(data + sent),
                             static_cast<int>(size - sent), 0);
#else
    const ssize_t count = ::send(socket, data + sent, size - sent, MSG_NOSIGNAL);
#endif
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool send_all(const SocketHandle socket, const std::string_view text) noexcept {
  return send_all(socket, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string trim_ascii(std::string value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(),
                       [](const unsigned char character) { return std::isspace(character); });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char character) {
        return std::isspace(character);
      }).base();
  if (first >= last)
    return {};
  return {first, last};
}

struct HttpRequest final {
  std::string method;
  std::string target;
  std::unordered_map<std::string, std::string> headers;
};

bool parse_http_request(const std::string &raw, HttpRequest &request) {
  const std::size_t request_end = raw.find("\r\n");
  if (request_end == std::string::npos)
    return false;
  std::istringstream first_line(raw.substr(0, request_end));
  std::string version;
  if (!(first_line >> request.method >> request.target >> version) || version != "HTTP/1.1")
    return false;

  std::size_t line_begin = request_end + 2;
  while (line_begin < raw.size()) {
    const std::size_t line_end = raw.find("\r\n", line_begin);
    if (line_end == std::string::npos || line_end == line_begin)
      break;
    const std::string line = raw.substr(line_begin, line_end - line_begin);
    const std::size_t separator = line.find(':');
    if (separator == std::string::npos)
      return false;
    request.headers[lower_ascii(line.substr(0, separator))] =
        trim_ascii(line.substr(separator + 1));
    line_begin = line_end + 2;
  }
  return true;
}

bool read_http_request(const SocketHandle socket, const std::atomic_bool &stop_requested,
                       const std::atomic_bool &server_shutdown, std::string &request) {
  set_receive_timeout(socket);
  std::array<char, 2048> buffer{};
  while (request.size() < kMaximumHttpRequestBytes && !stop_requested.load() &&
         !server_shutdown.load()) {
#if defined(_WIN32)
    const int count = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t count = ::recv(socket, buffer.data(), buffer.size(), 0);
#endif
    if (count > 0) {
      request.append(buffer.data(), static_cast<std::size_t>(count));
      if (request.find("\r\n\r\n") != std::string::npos)
        return true;
      continue;
    }
    if (count == 0)
      return false;
    if (retryable_socket_error())
      continue;
    return false;
  }
  return false;
}

std::uint32_t rotate_left(const std::uint32_t value, const unsigned count) noexcept {
  return (value << count) | (value >> (32U - count));
}

std::array<std::uint8_t, 20> sha1(const std::string_view input) {
  std::vector<std::uint8_t> message(input.begin(), input.end());
  const std::uint64_t bit_count = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80);
  while ((message.size() % 64U) != 56U)
    message.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
    message.push_back(static_cast<std::uint8_t>((bit_count >> shift) & 0xffU));

  std::uint32_t h0 = 0x67452301U;
  std::uint32_t h1 = 0xefcdab89U;
  std::uint32_t h2 = 0x98badcfeU;
  std::uint32_t h3 = 0x10325476U;
  std::uint32_t h4 = 0xc3d2e1f0U;
  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 80> words{};
    for (unsigned index = 0; index < 16; ++index) {
      const std::size_t offset = block + index * 4;
      words[index] = (static_cast<std::uint32_t>(message[offset]) << 24U) |
                     (static_cast<std::uint32_t>(message[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(message[offset + 2]) << 8U) |
                     static_cast<std::uint32_t>(message[offset + 3]);
    }
    for (unsigned index = 16; index < 80; ++index)
      words[index] = rotate_left(
          words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);

    std::uint32_t a = h0;
    std::uint32_t b = h1;
    std::uint32_t c = h2;
    std::uint32_t d = h3;
    std::uint32_t e = h4;
    for (unsigned index = 0; index < 80; ++index) {
      std::uint32_t function = 0;
      std::uint32_t constant = 0;
      if (index < 20) {
        function = (b & c) | ((~b) & d);
        constant = 0x5a827999U;
      } else if (index < 40) {
        function = b ^ c ^ d;
        constant = 0x6ed9eba1U;
      } else if (index < 60) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdcU;
      } else {
        function = b ^ c ^ d;
        constant = 0xca62c1d6U;
      }
      const std::uint32_t next = rotate_left(a, 5) + function + e + constant + words[index];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = next;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  const std::array<std::uint32_t, 5> words{h0, h1, h2, h3, h4};
  std::array<std::uint8_t, 20> digest{};
  for (unsigned index = 0; index < words.size(); ++index) {
    digest[index * 4] = static_cast<std::uint8_t>(words[index] >> 24U);
    digest[index * 4 + 1] = static_cast<std::uint8_t>(words[index] >> 16U);
    digest[index * 4 + 2] = static_cast<std::uint8_t>(words[index] >> 8U);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(words[index]);
  }
  return digest;
}

std::string base64(const std::uint8_t *data, const std::size_t size) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve((size + 2) / 3 * 4);
  for (std::size_t index = 0; index < size; index += 3) {
    const std::size_t remaining = size - index;
    const std::uint32_t value =
        static_cast<std::uint32_t>(data[index]) << 16U |
        (remaining > 1 ? static_cast<std::uint32_t>(data[index + 1]) << 8U : 0U) |
        (remaining > 2 ? data[index + 2] : 0U);
    encoded.push_back(alphabet[(value >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(value >> 12U) & 0x3fU]);
    encoded.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3fU] : '=');
    encoded.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
  }
  return encoded;
}

std::string websocket_accept(const std::string_view key) {
  std::string input(key);
  input.append(kWebSocketMagic.data(), kWebSocketMagic.size());
  const auto digest = sha1(input);
  return base64(digest.data(), digest.size());
}

bool websocket_upgrade_request(const HttpRequest &request) {
  const auto upgrade = request.headers.find("upgrade");
  const auto connection = request.headers.find("connection");
  const auto key = request.headers.find("sec-websocket-key");
  const auto version = request.headers.find("sec-websocket-version");
  if (upgrade == request.headers.end() || connection == request.headers.end() ||
      key == request.headers.end() || version == request.headers.end())
    return false;
  return lower_ascii(upgrade->second) == "websocket" &&
         lower_ascii(connection->second).find("upgrade") != std::string::npos &&
         version->second == "13" && !key->second.empty();
}

bool send_http_response(const SocketHandle socket, const int status, const std::string_view reason,
                        const std::string_view content_type, const std::string_view body) {
  std::string response;
  response.reserve(128 + body.size());
  response.append("HTTP/1.1 ");
  response.append(std::to_string(status));
  response.push_back(' ');
  response.append(reason);
  response.append("\r\nContent-Type: ");
  response.append(content_type);
  response.append("\r\nContent-Length: ");
  response.append(std::to_string(body.size()));
  response.append("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  response.append(body);
  return send_all(socket, response);
}

bool send_websocket_handshake(const SocketHandle socket, const std::string_view key) {
  const std::string accept = websocket_accept(key);
  std::string response{"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Accept: "};
  response.append(accept);
  response.append("\r\n\r\n");
  return send_all(socket, response);
}

bool send_websocket_frame(const SocketHandle socket, const std::uint8_t opcode,
                          const std::string_view payload) noexcept {
  std::array<std::uint8_t, 10> header{};
  std::size_t header_size = 2;
  header[0] = static_cast<std::uint8_t>(0x80U | (opcode & 0x0fU));
  if (payload.size() < 126U) {
    header[1] = static_cast<std::uint8_t>(payload.size());
  } else if (payload.size() <= std::numeric_limits<std::uint16_t>::max()) {
    header[1] = 126U;
    header[2] = static_cast<std::uint8_t>(payload.size() >> 8U);
    header[3] = static_cast<std::uint8_t>(payload.size());
    header_size = 4;
  } else {
    header[1] = 127U;
    header_size = 10;
    std::uint64_t size = payload.size();
    for (unsigned index = 0; index < 8; ++index) {
      header[2 + index] = static_cast<std::uint8_t>(size >> (56U - index * 8U));
    }
  }
  return send_all(socket, header.data(), header_size) && send_all(socket, payload);
}

bool send_websocket_close(const SocketHandle socket, const std::uint16_t code) noexcept {
  const std::array<char, 2> payload{static_cast<char>(code >> 8U), static_cast<char>(code)};
  return send_websocket_frame(socket, 0x8, std::string_view(payload.data(), payload.size()));
}

class WebSocketLineBuffer final : public std::streambuf {
public:
  WebSocketLineBuffer(SocketHandle socket, const std::atomic_bool &stop_requested,
                      const std::atomic_bool &server_shutdown) noexcept
      : socket_(socket), stop_requested_(&stop_requested), server_shutdown_(&server_shutdown) {}

  [[nodiscard]] bool good() const noexcept { return good_; }

protected:
  std::streamsize xsputn(const char *data, const std::streamsize size) override {
    if (!good_ || size < 0)
      return 0;
    for (std::streamsize index = 0; index < size; ++index) {
      if (!append(data[index]))
        return 0;
    }
    return size;
  }

  int_type overflow(const int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof()))
      return sync() == 0 ? traits_type::not_eof(character) : traits_type::eof();
    return append(traits_type::to_char_type(character)) ? character : traits_type::eof();
  }

  int sync() override { return pending_.empty() ? 0 : -1; }

private:
  bool append(const char character) {
    if (!good_ || stop_requested_->load() || server_shutdown_->load()) {
      good_ = false;
      return false;
    }
    pending_.push_back(character);
    if (character != '\n')
      return true;
    pending_.pop_back();
    good_ = send_websocket_frame(socket_, 0x1, pending_);
    pending_.clear();
    return good_;
  }

  SocketHandle socket_;
  const std::atomic_bool *stop_requested_;
  const std::atomic_bool *server_shutdown_;
  std::string pending_;
  bool good_{true};
};

class WebSocketPixelFrameSink final : public local_argb::PixelFrameSink {
public:
  WebSocketPixelFrameSink(const ReplayClock &clock, const SocketHandle socket,
                          const std::atomic_bool &stop_requested,
                          const std::atomic_bool &server_shutdown) noexcept
      : stream_buffer_(socket, stop_requested, server_shutdown), stream_(&stream_buffer_),
        encoder_(clock, stream_) {}

  bool write(const local_argb::PixelFrame &frame) noexcept override {
    return encoder_.write(frame) && stream_buffer_.good();
  }

  [[nodiscard]] bool write_header() noexcept {
    return encoder_.write_header() && stream_buffer_.good();
  }

  [[nodiscard]] bool write_end() noexcept { return encoder_.write_end() && stream_buffer_.good(); }

private:
  WebSocketLineBuffer stream_buffer_;
  std::ostream stream_;
  JsonlPixelFrameSink encoder_;
};

constexpr std::string_view kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GVRET LED Emulator</title><link rel="stylesheet" href="/style.css"></head>
<body><main><h1>GVRET LED Emulator</h1><p id="status">Connecting to local replay…</p>
<pre id="protocol" aria-live="polite"></pre></main><script src="/app.js"></script></body>
</html>
)HTML";

constexpr std::string_view kStyleCss =
    R"CSS(:root{font-family:system-ui,sans-serif;color:#e8edf2;background:#101418}
body{margin:0;min-height:100vh;display:grid;place-items:center}main{max-width:42rem;padding:2rem}
h1{font-size:1.6rem}#status{color:#9ac7ff}pre{white-space:pre-wrap;color:#b7c4cc}
)CSS";

constexpr std::string_view kAppJs = R"JS((()=>{
const status=document.querySelector('#status');const protocol=document.querySelector('#protocol');
const scheme=location.protocol==='https:'?'wss':'ws';const socket=new WebSocket(`${scheme}://${location.host}/ws`);
socket.onopen=()=>{status.textContent='Connected to local replay';};
socket.onmessage=event=>{try{const message=JSON.parse(event.data);if(message.type==='header')status.textContent=`Replay connected (${message.pixel_count} pixels)`;if(message.type==='pixels')protocol.textContent=`Latest frame at ${message.timestamp_us} µs`;if(message.type==='end')status.textContent='Replay complete';}catch(_){status.textContent='Invalid replay message';}};
socket.onerror=()=>{status.textContent='Local replay connection failed';};socket.onclose=()=>{if(status.textContent==='Connected to local replay')status.textContent='Replay disconnected';};
})();
)JS";

bool serve_websocket(const SocketHandle socket, const std::vector<gvret::TimedCanFrame> &frames,
                     const WebEmulatorOptions &options, const std::atomic_bool &stop_requested,
                     const std::atomic_bool &server_shutdown) {
  ReplayClock clock;
  WebSocketPixelFrameSink pixels{clock, socket, stop_requested, server_shutdown};
  if (!pixels.write_header())
    return false;

  LocalArgbOutputStage output{pixels};
  ReplayScheduleOptions schedule = options.schedule;
  schedule.end_time_us = frames.empty() ? 0 : frames.back().relative_time_us;
  const ReplayScheduleResult replay = run_replay(frames, clock, output, schedule);
  if (!replay.ok() || stop_requested.load() || server_shutdown.load())
    return false;
  return pixels.write_end() && send_websocket_close(socket, 1000);
}

bool serve_http_client(const SocketHandle socket, const std::vector<gvret::TimedCanFrame> &frames,
                       const WebEmulatorOptions &options, const std::atomic_bool &stop_requested,
                       const std::atomic_bool &server_shutdown) {
  std::string raw_request;
  if (!read_http_request(socket, stop_requested, server_shutdown, raw_request))
    return false;
  HttpRequest request;
  if (!parse_http_request(raw_request, request))
    return send_http_response(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                              "bad request\n");
  if (request.method != "GET")
    return send_http_response(socket, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                              "method not allowed\n");

  if (request.target == "/ws") {
    const auto key = request.headers.find("sec-websocket-key");
    if (!websocket_upgrade_request(request) || key == request.headers.end())
      return send_http_response(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                                "websocket upgrade required\n");
    if (!send_websocket_handshake(socket, key->second))
      return false;
    return serve_websocket(socket, frames, options, stop_requested, server_shutdown);
  }

  if (request.target == "/" || request.target == "/index.html")
    return send_http_response(socket, 200, "OK", "text/html; charset=utf-8", kIndexHtml);
  if (request.target == "/style.css")
    return send_http_response(socket, 200, "OK", "text/css; charset=utf-8", kStyleCss);
  if (request.target == "/app.js")
    return send_http_response(socket, 200, "OK", "text/javascript; charset=utf-8", kAppJs);
  return send_http_response(socket, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
}

bool loopback_address(const WebEmulatorOptions &options) noexcept {
  if (options.ipv6)
    return options.bind_address == "::1" || options.bind_address == "127.0.0.1";
  return options.bind_address == "127.0.0.1";
}

} // namespace

WebEmulatorServer::WebEmulatorServer(std::vector<gvret::TimedCanFrame> frames,
                                     WebEmulatorOptions options)
    : frames_(std::move(frames)), options_(std::move(options)) {
  if (options_.bind_address == "::1")
    options_.ipv6 = true;
  if (options_.ipv6 && options_.bind_address == "127.0.0.1")
    options_.bind_address = "::1";
}

WebEmulatorServer::~WebEmulatorServer() noexcept {
  request_shutdown();
  const SocketHandle listener = socket_from_storage(listener_.exchange(-1));
  close_socket(listener);
}

bool WebEmulatorServer::start() noexcept {
  if (valid_socket(socket_from_storage(listener_.load()))) {
    error_ = "emulator server is already started";
    return false;
  }
  if (!loopback_address(options_)) {
    error_ = "emulator bind address must be loopback (127.0.0.1 or ::1)";
    return false;
  }

#if defined(_WIN32)
  static const bool winsock_started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!winsock_started) {
    error_ = "unable to start Winsock";
    return false;
  }
#endif

  const int family = options_.ipv6 ? AF_INET6 : AF_INET;
  const SocketHandle socket = ::socket(family, SOCK_STREAM, 0);
  if (!valid_socket(socket)) {
    error_ = "unable to create loopback socket";
    return false;
  }
  const int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));

  int bind_result = -1;
  if (options_.ipv6) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(options_.port);
    ::inet_pton(AF_INET6, "::1", &address.sin6_addr);
    const int only_v6 = 1;
    ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&only_v6),
                 sizeof(only_v6));
    bind_result = ::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  } else {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    bind_result = ::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  }
  if (bind_result != 0 || ::listen(socket, 8) != 0) {
    close_socket(socket);
    error_ = "unable to bind loopback listener";
    return false;
  }
  if (!set_nonblocking(socket)) {
    close_socket(socket);
    error_ = "unable to configure loopback listener";
    return false;
  }

  std::uint16_t bound_port = 0;
  if (options_.ipv6) {
    sockaddr_in6 address{};
    socklen_t length = sizeof(address);
    if (::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
      close_socket(socket);
      error_ = "unable to inspect loopback listener";
      return false;
    }
    bound_port = ntohs(address.sin6_port);
  } else {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
      close_socket(socket);
      error_ = "unable to inspect loopback listener";
      return false;
    }
    bound_port = ntohs(address.sin_port);
  }

  bound_port_ = bound_port;
  listener_.store(socket_to_storage(socket));
  url_ = options_.ipv6 ? "http://[::1]:" + std::to_string(bound_port_) + "/"
                       : "http://127.0.0.1:" + std::to_string(bound_port_) + "/";
  error_.clear();
  return true;
}

bool WebEmulatorServer::serve(const std::atomic_bool &stop_requested) noexcept {
  const SocketHandle listener = socket_from_storage(listener_.load());
  if (!valid_socket(listener)) {
    error_ = "emulator server is not started";
    return false;
  }

  bool success = true;
  while (!stop_requested.load() && !shutdown_requested_.load()) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    const SocketHandle client = ::accept(listener, reinterpret_cast<sockaddr *>(&address), &length);
    if (!valid_socket(client)) {
      if (stop_requested.load() || shutdown_requested_.load())
        continue;
      if (retryable_socket_error()) {
        std::this_thread::yield();
        continue;
      }
      error_ = "loopback listener accept failed";
      success = false;
      break;
    }
    if (!set_blocking(client)) {
      close_socket(client);
      continue;
    }
    active_client_.store(socket_to_storage(client));
    try {
      (void)serve_http_client(client, frames_, options_, stop_requested, shutdown_requested_);
    } catch (...) {
      success = false;
      error_ = "loopback client handling failed";
    }
    shutdown_socket(client);
    close_socket(client);
    active_client_.store(-1);
  }

  shutdown_socket(listener);
  close_socket(listener);
  listener_.store(-1);
  return success;
}

void WebEmulatorServer::request_shutdown() noexcept {
  shutdown_requested_.store(true);
  shutdown_socket(socket_from_storage(listener_.load()));
  shutdown_socket(socket_from_storage(active_client_.load()));
}

} // namespace replay
