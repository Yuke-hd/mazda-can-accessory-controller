#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "replay/local_argb_stage.hpp"
#include "replay/pixel_frame_output.hpp"
#include "replay/scheduler.hpp"
#include "replay/web_emulator.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

std::vector<std::string> websocket_text_payloads(const std::string &response) {
  std::vector<std::string> payloads;
  const std::size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos)
    return payloads;
  std::size_t offset = header_end + 4;
  while (offset + 2 <= response.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(response[offset]);
    const std::uint8_t second = static_cast<std::uint8_t>(response[offset + 1]);
    offset += 2;
    if ((first & 0x80U) == 0 || (second & 0x80U) != 0)
      return {};
    std::uint64_t length = second & 0x7fU;
    if (length == 126U) {
      if (offset + 2 > response.size())
        return {};
      length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(response[offset])) << 8U) |
               static_cast<std::uint8_t>(response[offset + 1]);
      offset += 2;
    } else if (length == 127U) {
      if (offset + 8 > response.size())
        return {};
      length = 0;
      for (unsigned index = 0; index < 8; ++index)
        length = (length << 8U) | static_cast<std::uint8_t>(response[offset + index]);
      offset += 8;
    }
    if (length > response.size() - offset)
      return {};
    if ((first & 0x0fU) == 0x1U)
      payloads.emplace_back(response.data() + offset, static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    if ((first & 0x0fU) == 0x8U)
      break;
  }
  return payloads;
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

  const int websocket_client = connect_loopback(server);
  send_request(websocket_client, websocket_request(server, "keep-alive, Upgrade"));
  const std::string response = read_until_close(websocket_client);
  CHECK(response.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
  CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  const auto messages = websocket_text_payloads(response);
  REQUIRE(messages.size() >= 3);
  CHECK(messages.front() == "{\"type\":\"header\",\"version\":1,\"pixel_count\":100}");
  CHECK(messages[1].find("{\"type\":\"pixels\",\"timestamp_us\":0") == 0);
  CHECK(messages.back() == "{\"type\":\"end\"}");

  ::shutdown(websocket_client, SHUT_RDWR);
  ::close(websocket_client);
  server.request_shutdown();
  stop_requested.store(true);
  serving.join();
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
  const int client = connect_loopback(server);
  send_request(client, websocket_request(server));
  const auto messages = websocket_text_payloads(read_until_close(client, std::chrono::seconds(5)));
  ::close(client);
  stop_server(server, stop_requested, serving);

  // The browser receives byte-identical D1 records to the production sink.
  CHECK(messages == expected);
  REQUIRE(expected.size() > 3);
  const std::string black = "[0,0,0]";
  std::string all_black = "\"pixels\":[" + black;
  for (int index = 1; index < 100; ++index)
    all_black += "," + black;
  CHECK(any_record_contains(messages, all_black + "]}"));
  CHECK(any_record_contains(messages, "[0,16,16]"));  // RPM fill cyan.
  CHECK(any_record_contains(messages, "[0,8,8]"));    // Half-bright fill edge.
  CHECK(any_record_contains(messages, "[16,0,0]"));   // Red zone.
  CHECK(any_record_contains(messages, "[128,16,0]")); // Turn and hazard amber.
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
