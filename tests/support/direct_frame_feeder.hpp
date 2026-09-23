#pragma once

#include <cstddef>

#include "vehicle_core/frame.hpp"

namespace test_support {

// Test-only direct injection seam. It models acquisition delivery without a
// capture parser, file format, or production CAN ownership.
class DirectFrameFeeder final {
public:
  template <typename Handler> void feed(const vehicle_core::RawCanFrame &frame, Handler &&handler) {
    handler(frame);
    ++delivered_;
  }

  [[nodiscard]] std::size_t delivered() const noexcept { return delivered_; }

private:
  std::size_t delivered_{0};
};

} // namespace test_support
