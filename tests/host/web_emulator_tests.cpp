#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/scheduler.hpp"
#include "replay/web_emulator.hpp"
#include "replay/websocket_client_frames.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

gvret::TimedCanFrame frame_at(const vehicle_core::Microseconds timestamp) {
  gvret::TimedCanFrame frame;
  frame.relative_time_us = timestamp;
  frame.frame.timestamp_us = timestamp;
  frame.frame.identifier = 0x202;
  frame.frame.dlc = 8;
  frame.frame.data[0] = 0x32;
  frame.frame.data[1] = 0xC8;
  return frame;
}

int connect_loopback(const replay::WebEmulatorServer &server) {
  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(client >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server.port());
  REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  REQUIRE(::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
  return client;
}

std::string read_until_close(const int socket,
                             const std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  constexpr std::size_t kMaximumResponseBytes = 4 * 1024 * 1024;
  std::string output;
  char buffer[4096];
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline && output.size() < kMaximumResponseBytes) {
    const ssize_t count = ::recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) {
      const std::size_t remaining = kMaximumResponseBytes - output.size();
      output.append(buffer, std::min(remaining, static_cast<std::size_t>(count)));
      continue;
    }
    if (count == 0)
      break;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return output;
}

void send_request(const int socket, const std::string &request) {
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t count = ::send(socket, request.data() + sent, request.size() - sent, 0);
    REQUIRE(count > 0);
    sent += static_cast<std::size_t>(count);
  }
}

std::string local_host(const replay::WebEmulatorServer &server) {
  return "127.0.0.1:" + std::to_string(server.port());
}

std::string local_origin(const replay::WebEmulatorServer &server) {
  return "http://" + local_host(server);
}

std::string websocket_request(const replay::WebEmulatorServer &server,
                              const std::string_view connection = "Upgrade",
                              const std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==",
                              const std::string_view origin = "",
                              const std::string_view host = "") {
  const std::string request_origin = origin.empty() ? local_origin(server) : std::string(origin);
  const std::string request_host = host.empty() ? local_host(server) : std::string(host);
  return "GET /ws HTTP/1.1\r\nHost: " + request_host + "\r\nOrigin: " + request_origin +
         "\r\nUpgrade: websocket\r\nConnection: " + std::string(connection) +
         "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + std::string(key) + "\r\n\r\n";
}

struct ServerFrame final {
  std::uint8_t opcode{0};
  std::string payload;
};

// A minimal browser: completes the WebSocket handshake, sends masked client
// frames, and reads the server's text records one at a time.
class BrowserClient final {
public:
  explicit BrowserClient(const replay::WebEmulatorServer &server,
                         const std::string_view connection = "Upgrade")
      : socket_(connect_loopback(server)) {
    send_request(socket_, websocket_request(server, connection));
  }
  BrowserClient(const BrowserClient &) = delete;
  BrowserClient &operator=(const BrowserClient &) = delete;
  ~BrowserClient() {
    ::shutdown(socket_, SHUT_RDWR);
    ::close(socket_);
  }

  void send_frame(const std::uint8_t first_byte, const std::string_view payload,
                  const bool masked = true) {
    const std::array<std::uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};
    std::string frame;
    frame.push_back(static_cast<char>(first_byte));
    const std::uint8_t mask_bit = masked ? 0x80U : 0U;
    if (payload.size() < 126U) {
      frame.push_back(static_cast<char>(mask_bit | payload.size()));
    } else {
      frame.push_back(static_cast<char>(mask_bit | 126U));
      frame.push_back(static_cast<char>(payload.size() >> 8U));
      frame.push_back(static_cast<char>(payload.size() & 0xffU));
    }
    if (masked)
      frame.append(mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index)
      frame.push_back(static_cast<char>(payload[index] ^ (masked ? mask[index % 4] : 0U)));
    send_request(socket_, frame);
  }

  void send_text(const std::string_view text) { send_frame(0x81, text); }

  void send_control(const std::string_view command) {
    send_text("{\"type\":\"control\",\"command\":\"" + std::string(command) + "\"}");
  }

  void send_rate(const std::string_view rate) {
    send_text("{\"type\":\"control\",\"command\":\"rate\",\"rate\":" + std::string(rate) + "}");
  }

  // The next server frame, or nullopt at the deadline or end of stream.
  std::optional<ServerFrame> next_frame(const std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (auto frame = take_frame())
        return frame;
      if (closed_ || std::chrono::steady_clock::now() >= deadline)
        return std::nullopt;
      char buffer[4096];
      const ssize_t count = ::recv(socket_, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (count > 0) {
        buffer_.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        closed_ = true;
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Text records up to and including the first one equal to last; REQUIREs
  // that it arrives within timeout.
  std::vector<std::string>
  read_through(const std::string_view last,
               const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::vector<std::string> records;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (auto frame = next_frame(deadline)) {
      if (frame->opcode != 0x1)
        continue;
      records.push_back(frame->payload);
      if (frame->payload == last)
        return records;
    }
    CAPTURE(last);
    FAIL("record did not arrive");
    return records;
  }

  // Every text record that arrives during window.
  std::vector<std::string> read_for(const std::chrono::milliseconds window) {
    std::vector<std::string> records;
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (auto frame = next_frame(deadline)) {
      if (frame->opcode == 0x1)
        records.push_back(frame->payload);
    }
    return records;
  }

  // The close code of the server's close frame, skipping text records.
  std::optional<std::uint16_t> read_close_code() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (auto frame = next_frame(deadline)) {
      if (frame->opcode == 0x8 && frame->payload.size() == 2)
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(frame->payload[0]) << 8U |
                                          static_cast<std::uint8_t>(frame->payload[1]));
    }
    return std::nullopt;
  }

  [[nodiscard]] const std::string &handshake() const noexcept { return handshake_; }

private:
  std::optional<ServerFrame> take_frame() {
    if (handshake_.empty()) {
      const std::size_t header_end = buffer_.find("\r\n\r\n");
      if (header_end == std::string::npos)
        return std::nullopt;
      handshake_ = buffer_.substr(0, header_end + 4);
      buffer_.erase(0, header_end + 4);
    }
    if (buffer_.size() < 2)
      return std::nullopt;
    const auto first = static_cast<std::uint8_t>(buffer_[0]);
    const auto second = static_cast<std::uint8_t>(buffer_[1]);
    REQUIRE((first & 0x80U) != 0);  // Server frames are never fragmented.
    REQUIRE((second & 0x80U) == 0); // or masked.
    std::size_t offset = 2;
    std::uint64_t length = second & 0x7fU;
    const std::size_t length_bytes = length == 126U ? 2 : (length == 127U ? 8 : 0);
    if (buffer_.size() < offset + length_bytes)
      return std::nullopt;
    if (length_bytes != 0) {
      length = 0;
      for (std::size_t index = 0; index < length_bytes; ++index)
        length = (length << 8U) | static_cast<std::uint8_t>(buffer_[offset + index]);
      offset += length_bytes;
    }
    if (buffer_.size() - offset < length)
      return std::nullopt;
    ServerFrame frame{static_cast<std::uint8_t>(first & 0x0fU),
                      buffer_.substr(offset, static_cast<std::size_t>(length))};
    buffer_.erase(0, offset + static_cast<std::size_t>(length));
    return frame;
  }

  int socket_;
  std::string buffer_;
  std::string handshake_;
  bool closed_{false};
};

bool is_playback_record(const std::string &record) {
  return record.rfind("{\"type\":\"playback\"", 0) == 0;
}

// Only the D1 stream: drops the playback acknowledgements that interleave it.
std::vector<std::string> d1_records(std::vector<std::string> records) {
  records.erase(std::remove_if(records.begin(), records.end(), is_playback_record), records.end());
  return records;
}

constexpr std::string_view kHeaderRecord{"{\"type\":\"header\",\"version\":1,\"pixel_count\":100}"};
constexpr std::string_view kEndRecord{"{\"type\":\"end\"}"};
constexpr std::string_view kRestartRecord{"{\"type\":\"restart\"}"};

std::string playback_state(const bool paused, const std::string_view rate) {
  return "{\"type\":\"playback\",\"paused\":" + std::string(paused ? "true" : "false") +
         ",\"rate\":" + std::string(rate) + "}";
}

std::string all_black_pixels() {
  std::string pixels = "\"pixels\":[[0,0,0]";
  for (int index = 1; index < 100; ++index)
    pixels += ",[0,0,0]";
  return pixels + "]}";
}

bool is_pixel_record(const std::string &record) {
  return record.rfind("{\"type\":\"pixels\"", 0) == 0;
}

void stop_server(replay::WebEmulatorServer &server, std::atomic_bool &stop_requested,
                 std::thread &serving) {
  server.request_shutdown();
  stop_requested.store(true);
  serving.join();
}

gvret::TimedCanFrame vehicle_frame(const vehicle_core::Microseconds timestamp,
                                   const std::uint32_t identifier,
                                   const std::initializer_list<std::uint8_t> bytes) {
  gvret::TimedCanFrame frame;
  frame.relative_time_us = timestamp;
  frame.frame.timestamp_us = timestamp;
  frame.frame.identifier = identifier;
  frame.frame.dlc = static_cast<std::uint8_t>(bytes.size());
  std::copy(bytes.begin(), bytes.end(), frame.frame.data.begin());
  return frame;
}

gvret::TimedCanFrame engine_rpm(const vehicle_core::Microseconds timestamp,
                                const std::uint16_t raw_rpm_times_four) {
  return vehicle_frame(timestamp, 0x202,
                       {static_cast<std::uint8_t>(raw_rpm_times_four >> 8),
                        static_cast<std::uint8_t>(raw_rpm_times_four & 0xff), 0, 0, 0, 0, 0, 0});
}

gvret::TimedCanFrame turn_switch(const vehicle_core::Microseconds timestamp,
                                 const std::uint8_t state) {
  return vehicle_frame(timestamp, 0x091, {0, state, 0, 0, 0, 0, 0, 0});
}

// The production scenario from replay_output_tests: RPM fill and red zone,
// both turn directions, hazard, and the return to the RPM baseline.
std::vector<gvret::TimedCanFrame> production_scenario() {
  return {
      engine_rpm(0, 6'500),        engine_rpm(50'000, 10'000), engine_rpm(100'000, 13'000),
      engine_rpm(200'000, 30'000), turn_switch(300'000, 0x20), turn_switch(400'000, 0x10),
      turn_switch(500'000, 0x04),  turn_switch(600'000, 0x00),
  };
}

// D1 records produced by the production pipeline without any transport: the
// local ARGB stage writing through the JSONL PixelFrameSink.
std::vector<std::string> production_d1_records(const std::vector<gvret::TimedCanFrame> &frames,
                                               replay::ReplayScheduleOptions schedule) {
  replay::ReplayClock clock;
  std::ostringstream output;
  replay::JsonlPixelFrameSink pixels{clock, output};
  REQUIRE(pixels.write_header());
  replay::LocalArgbOutputStage stage{pixels};
  // Mirrors serve_websocket in lib/replay/src/web_emulator.cpp; keep in sync.
  schedule.end_time_us = frames.back().relative_time_us;
  REQUIRE(replay::run_replay(frames, clock, stage, schedule).ok());
  REQUIRE(pixels.write_end());

  std::vector<std::string> records;
  std::istringstream lines(output.str());
  for (std::string line; std::getline(lines, line);)
    records.push_back(line);
  return records;
}

bool any_record_contains(const std::vector<std::string> &records, const std::string_view text) {
  return std::any_of(records.begin(), records.end(), [text](const std::string &record) {
    return record.find(text) != std::string::npos;
  });
}

} // namespace

TEST_CASE("web emulator response reader keeps an absolute deadline while data streams") {
  int sockets[2]{};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  std::atomic_bool writer_stop{false};
  std::thread writer([&] {
    const char byte = 'x';
    while (!writer_stop.load()) {
      if (::send(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) < 0 && errno != EAGAIN &&
          errno != EWOULDBLOCK)
        break;
    }
  });

  const auto started = std::chrono::steady_clock::now();
  const std::string response = read_until_close(sockets[0], std::chrono::milliseconds(50));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK_FALSE(response.empty());
  CHECK(elapsed < std::chrono::seconds(1));

  writer_stop.store(true);
  ::shutdown(sockets[0], SHUT_RDWR);
  ::shutdown(sockets[1], SHUT_RDWR);
  writer.join();
  ::close(sockets[0]);
  ::close(sockets[1]);
}

TEST_CASE("web emulator defaults to an IPv4 loopback URL") {
  replay::WebEmulatorServer server({frame_at(0)}, {});
  CHECK(server.options().ipv6 == false);
  CHECK(server.options().bind_address == "127.0.0.1");
  REQUIRE(server.start());
  CHECK(server.url().find("http://127.0.0.1:") == 0);
  CHECK(server.port() != 0);
  server.request_shutdown();
}

TEST_CASE("web emulator refuses a remote bind address") {
  replay::WebEmulatorOptions options;
  options.bind_address = "0.0.0.0";
  replay::WebEmulatorServer server({frame_at(0)}, options);
  CHECK_FALSE(server.start());
  CHECK(server.error().find("loopback") != std::string::npos);
}

TEST_CASE("web emulator serves local assets and streams D1 over WebSocket") {
  replay::WebEmulatorServer server({frame_at(0)}, {});
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::thread serving([&] { (void)server.serve(stop_requested); });

  const int static_client = connect_loopback(server);
  send_request(static_client, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  const std::string static_response = read_until_close(static_client);
  CHECK(static_response.find("HTTP/1.1 200 OK") != std::string::npos);
  CHECK(static_response.find("/app.js") != std::string::npos);
  CHECK(static_response.find("https://") == std::string::npos);
  ::close(static_client);

  {
    BrowserClient browser(server, "keep-alive, Upgrade");
    const auto messages = browser.read_through(kEndRecord);
    CHECK(browser.handshake().find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
    CHECK(browser.handshake().find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
          std::string::npos);
    REQUIRE(messages.size() >= 4);
    CHECK(messages[0] == kHeaderRecord);
    CHECK(messages[1] == playback_state(false, "1"));
    CHECK(messages[2].find("{\"type\":\"pixels\",\"timestamp_us\":0") == 0);
    CHECK(messages.back() == kEndRecord);
  }
  stop_server(server, stop_requested, serving);
}

TEST_CASE("web emulator streams production PixelFrames for RPM, turn, hazard, and red zone") {
  const std::vector<gvret::TimedCanFrame> scenario = production_scenario();
  replay::WebEmulatorOptions options;
  options.schedule.poll_period_us = 50'000;
  const std::vector<std::string> expected = production_d1_records(scenario, options.schedule);

  replay::WebEmulatorServer server(scenario, options);
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::thread serving([&] { (void)server.serve(stop_requested); });
  std::vector<std::string> messages;
  {
    BrowserClient browser(server);
    messages = d1_records(browser.read_through(kEndRecord));
  }
  stop_server(server, stop_requested, serving);

  // The browser receives byte-identical D1 records to the production sink.
  CHECK(messages == expected);
  REQUIRE(expected.size() > 3);
  CHECK(any_record_contains(messages, all_black_pixels()));
  CHECK(any_record_contains(messages, "[0,16,16]"));  // RPM fill cyan.
  CHECK(any_record_contains(messages, "[0,8,8]"));    // Half-bright fill edge.
  CHECK(any_record_contains(messages, "[16,0,0]"));   // Red zone.
  CHECK(any_record_contains(messages, "[128,16,0]")); // Turn and hazard amber.
}

// A server replaying the production scenario to one browser at a time.
class ProductionReplay final {
public:
  ProductionReplay() : server_(production_scenario(), production_options()) {
    REQUIRE(server_.start());
    serving_ = std::thread([this] { (void)server_.serve(stop_requested_); });
  }
  ProductionReplay(const ProductionReplay &) = delete;
  ProductionReplay &operator=(const ProductionReplay &) = delete;
  ~ProductionReplay() { stop_server(server_, stop_requested_, serving_); }

  [[nodiscard]] const replay::WebEmulatorServer &server() const noexcept { return server_; }

  // What every playback must deliver, whatever the rate, pauses, or restarts.
  [[nodiscard]] static std::vector<std::string> expected() {
    return production_d1_records(production_scenario(), production_options().schedule);
  }

private:
  static replay::WebEmulatorOptions production_options() {
    replay::WebEmulatorOptions options;
    options.schedule.poll_period_us = 50'000;
    return options;
  }

  replay::WebEmulatorServer server_;
  std::atomic_bool stop_requested_{false};
  std::thread serving_;
};

TEST_CASE("web emulator rate changes delivery timing but never the D1 frame sequence") {
  const auto expected = ProductionReplay::expected();
  const auto scenario_duration = std::chrono::milliseconds(600);
  for (const auto &rate_and_scale : std::vector<std::pair<std::string, double>>{
           {"0.25", 0.25}, {"0.5", 0.5}, {"1", 1.0}, {"2", 2.0}, {"5", 5.0}}) {
    const std::string &rate = rate_and_scale.first;
    const double scale = rate_and_scale.second;
    CAPTURE(rate);
    ProductionReplay replay;
    BrowserClient browser(replay.server());
    auto records = browser.read_through(kHeaderRecord);
    const auto started = std::chrono::steady_clock::now();
    browser.send_rate(rate);
    const auto rest = browser.read_through(kEndRecord, std::chrono::seconds(10));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    records.insert(records.end(), rest.begin(), rest.end());

    CHECK(std::find(records.begin(), records.end(), playback_state(false, rate)) != records.end());
    CHECK(d1_records(records) == expected);
    // Replay time before the acknowledgement plays at 1x; allow for it.
    const auto minimum =
        std::chrono::duration_cast<std::chrono::milliseconds>(scenario_duration * 0.8 / scale);
    CHECK(elapsed >= minimum);
  }
}

TEST_CASE("web emulator pause stops replay time and resume continues from the same position") {
  ProductionReplay replay;
  BrowserClient browser(replay.server());
  auto records = browser.read_through(kHeaderRecord);
  const auto first_pixels = browser.read_for(std::chrono::milliseconds(150));
  records.insert(records.end(), first_pixels.begin(), first_pixels.end());
  browser.send_control("pause");
  const auto until_paused = browser.read_through(playback_state(true, "1"));
  records.insert(records.end(), until_paused.begin(), until_paused.end());

  // Nothing advances while paused: no pixel frames, no polls, no end.
  CHECK(browser.read_for(std::chrono::milliseconds(300)).empty());

  browser.send_control("play");
  const auto rest = browser.read_through(kEndRecord);
  CHECK(rest.front() == playback_state(false, "1"));
  records.insert(records.end(), rest.begin(), rest.end());
  CHECK(d1_records(records) == ProductionReplay::expected());
}

TEST_CASE("web emulator restart after the end replays from t=0") {
  const auto expected = ProductionReplay::expected();
  ProductionReplay replay;
  BrowserClient browser(replay.server());
  CHECK(d1_records(browser.read_through(kEndRecord)) == expected);

  // The connection stays open after the end so the browser can restart.
  browser.send_control("restart");
  const auto second = browser.read_through(kEndRecord);
  REQUIRE(second.size() > 2);
  CHECK(second[0] == kRestartRecord);
  CHECK(second[1] == kHeaderRecord);
  CHECK(second[2] == playback_state(false, "1"));
  CHECK(d1_records({second.begin() + 1, second.end()}) == expected);
}

TEST_CASE("web emulator restart while paused fails off to black and replays from t=0") {
  const auto expected = ProductionReplay::expected();
  ProductionReplay replay;
  BrowserClient browser(replay.server());
  (void)browser.read_through(kHeaderRecord);
  browser.send_rate("0.5");
  (void)browser.read_for(std::chrono::milliseconds(250));
  browser.send_control("pause");
  (void)browser.read_through(playback_state(true, "0.5"));

  browser.send_control("restart");
  const auto interrupted = browser.read_through(kRestartRecord);
  const auto last_pixels = std::find_if(interrupted.rbegin(), interrupted.rend(), is_pixel_record);
  REQUIRE(last_pixels != interrupted.rend());
  CHECK(last_pixels->find(all_black_pixels()) != std::string::npos);

  // A restart keeps the chosen rate but always resumes playing.
  const auto second = browser.read_through(kEndRecord, std::chrono::seconds(5));
  REQUIRE(second.size() > 1);
  CHECK(second[0] == kHeaderRecord);
  CHECK(second[1] == playback_state(false, "0.5"));
  CHECK(d1_records(second) == expected);
}

TEST_CASE("web emulator rejects invalid control messages and keeps playing") {
  ProductionReplay replay;
  BrowserClient browser(replay.server());
  (void)browser.read_through(kHeaderRecord);
  browser.send_rate("3");
  (void)browser.read_through("{\"type\":\"rejected\",\"reason\":\"unsupported_rate\"}");
  browser.send_text("not json");
  (void)browser.read_through("{\"type\":\"rejected\",\"reason\":\"malformed\"}");
  const auto rest = browser.read_through(kEndRecord);
  CHECK(std::none_of(rest.begin(), rest.end(), is_playback_record));
}

TEST_CASE("web emulator answers pings and closes on client close or protocol errors") {
  SUBCASE("ping") {
    ProductionReplay replay;
    BrowserClient browser(replay.server());
    (void)browser.read_through(kHeaderRecord);
    browser.send_frame(0x89, "beat");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<ServerFrame> pong;
    while (auto frame = browser.next_frame(deadline)) {
      if (frame->opcode == 0xA) {
        pong = frame;
        break;
      }
    }
    REQUIRE(pong.has_value());
    CHECK(pong->payload == "beat");
  }
  const std::vector<std::tuple<std::string, std::uint8_t, std::string, bool, std::uint16_t>>
      closing{
          {"client close", 0x88, std::string("\x03\xe8", 2), true, 1000},
          {"unmasked frame", 0x81, "{\"type\":\"control\",\"command\":\"pause\"}", false, 1002},
          {"binary frame", 0x82, "x", true, 1003},
          {"oversized text", 0x81, std::string(257, ' '), true, 1009},
      };
  for (const auto &closing_case : closing) {
    const auto &[name, first_byte, payload, masked, code] = closing_case;
    const std::string &case_name = name;
    CAPTURE(case_name);
    ProductionReplay replay;
    BrowserClient browser(replay.server());
    (void)browser.read_through(kHeaderRecord);
    browser.send_frame(first_byte, payload, masked);
    CHECK(browser.read_close_code() == std::optional<std::uint16_t>{code});
  }
}

TEST_CASE("client frame decoder unmasks text and answers control frames") {
  const auto masked = [](const std::uint8_t first_byte, const std::string &payload) {
    const std::array<std::uint8_t, 4> mask{1, 2, 3, 4};
    std::string frame{static_cast<char>(first_byte)};
    if (payload.size() < 126U) {
      frame.push_back(static_cast<char>(0x80U | payload.size()));
    } else {
      frame.push_back(static_cast<char>(0x80U | 126U));
      frame.push_back(static_cast<char>(payload.size() >> 8U));
      frame.push_back(static_cast<char>(payload.size() & 0xffU));
    }
    frame.append(mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index)
      frame.push_back(static_cast<char>(payload[index] ^ mask[index % 4]));
    return frame;
  };

  replay::ClientFrameDecoder decoder{16};
  const std::string bytes = masked(0x81, "hello") + masked(0x89, "p") + masked(0x8A, "");
  // Arrives one byte at a time.
  for (std::size_t index = 0; index + 1 < bytes.size(); ++index)
    decoder.append(bytes.substr(index, 1));
  auto first = decoder.next();
  REQUIRE(first.has_value());
  CHECK(first->kind == replay::ClientFrameKind::Text);
  CHECK(first->payload == "hello");
  auto second = decoder.next();
  REQUIRE(second.has_value());
  CHECK(second->kind == replay::ClientFrameKind::Ping);
  CHECK(second->payload == "p");
  CHECK_FALSE(decoder.next().has_value());
  decoder.append(bytes.substr(bytes.size() - 1));
  CHECK(decoder.next()->kind == replay::ClientFrameKind::Pong);

  SUBCASE("oversized text is refused from its header alone") {
    decoder.append(std::string{static_cast<char>(0x81), static_cast<char>(0xFE), 0, 17});
    CHECK(decoder.next()->kind == replay::ClientFrameKind::TooLarge);
    decoder.append(masked(0x81, "ignored"));
    CHECK_FALSE(decoder.next().has_value());
  }
  SUBCASE("fragments, reserved bits, unknown opcodes, and long control frames") {
    for (const std::string &frame : {masked(0x01, "a"), masked(0xC1, "a"), masked(0x83, "a"),
                                     masked(0x80, "a"), masked(0x89, std::string(126, 'x'))}) {
      replay::ClientFrameDecoder fresh{16};
      fresh.append(frame);
      CHECK(fresh.next()->kind == replay::ClientFrameKind::ProtocolError);
    }
  }
  SUBCASE("close ends the stream") {
    decoder.append(masked(0x88, std::string("\x03\xe8", 2)) + masked(0x81, "late"));
    CHECK(decoder.next()->kind == replay::ClientFrameKind::Close);
    CHECK_FALSE(decoder.next().has_value());
  }
  CHECK(replay::close_code_for(replay::ClientFrameKind::Text) == 0);
  CHECK(replay::close_code_for(replay::ClientFrameKind::TooLarge) == 1009);
}

TEST_CASE("web emulator rejects capture uploads") {
  replay::WebEmulatorServer server({frame_at(0)}, {});
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::thread serving([&] { (void)server.serve(stop_requested); });

  const int client = connect_loopback(server);
  send_request(client, "POST /ws HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc");
  CHECK(read_until_close(client).find("HTTP/1.1 405 Method Not Allowed") != std::string::npos);
  ::close(client);

  stop_server(server, stop_requested, serving);
}

TEST_CASE("web emulator requires exact local WebSocket origin and host") {
  const std::vector<std::pair<std::string, std::string>> invalid_locations{
      {"http://attacker.example", ""},
      {"", "attacker.example"},
  };
  for (const auto &[origin, host] : invalid_locations) {
    replay::WebEmulatorServer server({frame_at(0)}, {});
    REQUIRE(server.start());
    std::atomic_bool stop_requested{false};
    std::thread serving([&] { (void)server.serve(stop_requested); });

    const int client = connect_loopback(server);
    send_request(client,
                 websocket_request(server, "Upgrade", "dGhlIHNhbXBsZSBub25jZQ==", origin, host));
    const std::string response = read_until_close(client);
    CHECK(response.find("HTTP/1.1 403 Forbidden") != std::string::npos);
    CHECK(response.find("101 Switching Protocols") == std::string::npos);
    ::close(client);
    stop_server(server, stop_requested, serving);
  }
}

TEST_CASE("web emulator rejects invalid WebSocket connection tokens and keys") {
  const std::vector<std::pair<std::string, std::string>> invalid_requests{
      {"notupgrade", "dGhlIHNhbXBsZSBub25jZQ=="},
      {"Upgrade", "not-base64"},
      {"Upgrade", "dGhlIHNhbXBsZSBub25jZQ="},
  };
  for (const auto &[connection, key] : invalid_requests) {
    replay::WebEmulatorServer server({frame_at(0)}, {});
    REQUIRE(server.start());
    std::atomic_bool stop_requested{false};
    std::thread serving([&] { (void)server.serve(stop_requested); });

    const int client = connect_loopback(server);
    send_request(client, websocket_request(server, connection, key));
    const std::string response = read_until_close(client);
    CHECK(response.find("HTTP/1.1 400 Bad Request") != std::string::npos);
    CHECK(response.find("101 Switching Protocols") == std::string::npos);
    ::close(client);
    stop_server(server, stop_requested, serving);
  }
}

TEST_CASE("web emulator drops a stalled header before serving the next client") {
  replay::WebEmulatorServer server({frame_at(0)}, {});
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::thread serving([&] { (void)server.serve(stop_requested); });

  const int stalled_client = connect_loopback(server);
  send_request(stalled_client, "G");
  std::this_thread::sleep_for(std::chrono::milliseconds(650));

  const int valid_client = connect_loopback(server);
  send_request(valid_client,
               "GET / HTTP/1.1\r\nHost: " + local_host(server) + "\r\nConnection: close\r\n\r\n");
  const std::string response = read_until_close(valid_client);
  CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
  CHECK(response.find("/app.js") != std::string::npos);

  ::close(stalled_client);
  ::close(valid_client);
  stop_server(server, stop_requested, serving);
}

TEST_CASE("web emulator observes stop while a WebSocket peer is not reading") {
  std::vector<gvret::TimedCanFrame> frames;
  frames.reserve(2'000);
  for (std::size_t index = 0; index < 2'000; ++index) {
    auto frame = frame_at(static_cast<vehicle_core::Microseconds>(index * 1'000));
    frame.frame.data[0] = static_cast<std::uint8_t>((index % 2U) == 0U ? 0x32U : 0x64U);
    frames.push_back(frame);
  }
  replay::WebEmulatorOptions options;
  options.schedule.output_tick_period_us = 1'000;
  replay::WebEmulatorServer server(std::move(frames), options);
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::atomic_bool serving_done{false};
  std::thread serving([&] {
    (void)server.serve(stop_requested);
    serving_done.store(true);
  });

  const int client = connect_loopback(server);
  const int receive_buffer_size = 1'024;
  REQUIRE(::setsockopt(client, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char *>(&receive_buffer_size),
                       sizeof(receive_buffer_size)) == 0);
  send_request(client, websocket_request(server));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop_requested.store(true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!serving_done.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(serving_done.load());
  if (!serving_done.load())
    server.request_shutdown();
  ::shutdown(client, SHUT_RDWR);
  ::close(client);
  serving.join();
}

#else

TEST_CASE("web emulator exposes loopback defaults") {
  const replay::WebEmulatorServer server({}, {});
  CHECK(server.options().ipv6 == false);
  CHECK(server.options().bind_address == "127.0.0.1");
}

#endif

int main(int argc, char **argv) {
  doctest::Context context(argc, argv);
  return context.run();
}
