#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "gvret/replay_stream.hpp"
#include "replay/scheduler.hpp"

namespace replay {

// The emulator is deliberately restricted to loopback. It is a local browser
// adapter for replay output, not a network service or capture-upload endpoint.
struct WebEmulatorOptions final {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint32_t bus{0};
  bool ipv6{false};
  ReplayScheduleOptions schedule{};
};

// Serves the embedded browser assets and one-shot replay WebSocket sessions.
// The caller owns the input frames and controls the service lifetime through
// serve()'s stop flag or request_shutdown(). A new browser connection receives
// a fresh deterministic replay from time zero.
class WebEmulatorServer final {
public:
  WebEmulatorServer(std::vector<gvret::TimedCanFrame> frames, WebEmulatorOptions options = {});
  ~WebEmulatorServer() noexcept;

  WebEmulatorServer(const WebEmulatorServer &) = delete;
  WebEmulatorServer &operator=(const WebEmulatorServer &) = delete;
  WebEmulatorServer(WebEmulatorServer &&) = delete;
  WebEmulatorServer &operator=(WebEmulatorServer &&) = delete;

  // Bind the loopback listener. The default address is 127.0.0.1; ::1 is
  // accepted when options.ipv6 is true. No non-loopback address is accepted.
  [[nodiscard]] bool start() noexcept;

  // Accept HTTP and WebSocket clients until stop_requested or
  // request_shutdown() is observed. This method must run on one caller-owned
  // thread after start().
  [[nodiscard]] bool serve(const std::atomic_bool &stop_requested) noexcept;

  // Interrupts accept() and causes any in-progress replay sink to fail off.
  void request_shutdown() noexcept;

  [[nodiscard]] const WebEmulatorOptions &options() const noexcept { return options_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
  [[nodiscard]] const std::string &url() const noexcept { return url_; }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

private:
  std::vector<gvret::TimedCanFrame> frames_;
  WebEmulatorOptions options_;
  std::string url_;
  std::string error_;
  std::atomic_bool shutdown_requested_{false};
  std::atomic<std::intptr_t> active_client_{-1};
  std::atomic<std::intptr_t> listener_{-1};
  std::uint16_t bound_port_{0};
};

} // namespace replay
