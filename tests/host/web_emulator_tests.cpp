#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "replay/web_emulator.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
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

std::string read_available(const int socket) {
  std::string output;
  char buffer[4096];
  while (true) {
    const ssize_t count = ::recv(socket, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return output;
}

void send_request(const int socket, const std::string &request) {
  REQUIRE(::send(socket, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));
}

} // namespace

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
  const std::string static_response = read_available(static_client);
  CHECK(static_response.find("HTTP/1.1 200 OK") != std::string::npos);
  CHECK(static_response.find("/app.js") != std::string::npos);
  CHECK(static_response.find("https://") == std::string::npos);
  ::close(static_client);

  const int websocket_client = connect_loopback(server);
  send_request(websocket_client, "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
  const std::string response = read_available(websocket_client);
  CHECK(response.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
  CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  CHECK(response.find("{\"type\":\"header\",\"version\":1,\"pixel_count\":100}") !=
        std::string::npos);
  CHECK(response.find("{\"type\":\"pixels\",\"timestamp_us\":0") != std::string::npos);
  CHECK(response.find("{\"type\":\"end\"}") != std::string::npos);

  ::shutdown(websocket_client, SHUT_RDWR);
  ::close(websocket_client);
  server.request_shutdown();
  stop_requested.store(true);
  serving.join();
}

TEST_CASE("web emulator rejects capture uploads") {
  replay::WebEmulatorServer server({frame_at(0)}, {});
  REQUIRE(server.start());
  std::atomic_bool stop_requested{false};
  std::thread serving([&] { (void)server.serve(stop_requested); });

  const int client = connect_loopback(server);
  send_request(client, "POST /ws HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc");
  CHECK(read_available(client).find("HTTP/1.1 405 Method Not Allowed") != std::string::npos);
  ::close(client);

  server.request_shutdown();
  stop_requested.store(true);
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
