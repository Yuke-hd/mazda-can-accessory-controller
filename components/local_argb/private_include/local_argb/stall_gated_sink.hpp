#pragma once

#include <atomic>
#include <cstdint>

#include "local_argb/lighting_sink.hpp"

namespace local_argb::internal {

// Rejects lighting commands while a watched loop is stalled, so no publisher,
// such as a polled rule, can relight a strip that was failed off. The gate is
// paired with a cancellation generation; it re-emits nothing when it opens.
//
// shut() closes the gate for good before a controlled restart: a later
// progress resume cannot reopen it. in_flight() counts publishers between
// their gate check and their return, so the shutdown can wait until every
// racing publisher has written its trailing black.
class StallGatedSink final : public LightingSink {
public:
  explicit StallGatedSink(LightingSink &downstream) noexcept : downstream_(&downstream) {}

  bool publish(const LightingCommand &command) noexcept override {
    const InFlight in_flight{in_flight_};
    const auto epoch = transient_epoch();
    if (!is_open())
      return false;
    const bool accepted = downstream_->publish(command);
    if (is_open() && epoch == transient_epoch())
      return accepted;
    // The gate closed or its epoch changed during publication, including
    // faulted worker passes and explicit fail-off. Write black after any
    // racing command so it cannot stay lit.
    (void)downstream_->publish(LightingCommand{});
    return false;
  }

  [[nodiscard]] std::uint32_t transient_epoch() const noexcept override {
    return epoch_.load(std::memory_order_seq_cst);
  }
  void invalidate_transients() noexcept { epoch_.fetch_add(1, std::memory_order_seq_cst); }
  void close() noexcept {
    auto expected = State::Open;
    (void)state_.compare_exchange_strong(expected, State::Closed, std::memory_order_seq_cst);
    invalidate_transients();
  }
  void open() noexcept {
    // Starts captured while closed must also be invalid after resume.
    invalidate_transients();
    auto expected = State::Closed;
    (void)state_.compare_exchange_strong(expected, State::Open, std::memory_order_seq_cst);
  }
  // Closes the gate permanently; open() no longer has any effect.
  void shut() noexcept {
    state_.store(State::Shut, std::memory_order_seq_cst);
    invalidate_transients();
  }
  [[nodiscard]] bool is_shut() const noexcept {
    return state_.load(std::memory_order_seq_cst) == State::Shut;
  }
  // Publishers still inside publish(). Once the gate is shut and this reads
  // zero, no gated command can reach the downstream sink again.
  [[nodiscard]] std::uint32_t in_flight() const noexcept {
    return in_flight_.load(std::memory_order_seq_cst);
  }

private:
  enum class State : std::uint8_t { Open, Closed, Shut };

  class InFlight {
  public:
    explicit InFlight(std::atomic<std::uint32_t> &count) noexcept : count_(&count) {
      count_->fetch_add(1, std::memory_order_seq_cst);
    }
    ~InFlight() { count_->fetch_sub(1, std::memory_order_seq_cst); }
    InFlight(const InFlight &) = delete;
    InFlight &operator=(const InFlight &) = delete;

  private:
    std::atomic<std::uint32_t> *count_;
  };

  [[nodiscard]] bool is_open() const noexcept {
    return state_.load(std::memory_order_seq_cst) == State::Open;
  }

  LightingSink *downstream_;
  std::atomic<State> state_{State::Open};
  std::atomic<std::uint32_t> in_flight_{0};
  std::atomic<std::uint32_t> epoch_{0};
};

} // namespace local_argb::internal
