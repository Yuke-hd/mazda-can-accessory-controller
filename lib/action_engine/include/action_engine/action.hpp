#pragma once

#include <cstdint>

// Actions are the engine's only output. An action is a numeric identity chosen
// by configuration; its meaning (a lamp, a chime, a transport message) belongs
// entirely to the sinks that handle it.

namespace action_engine {

// Configuration-assigned action identity. Zero is reserved as invalid.
class ActionId final {
public:
  constexpr ActionId() noexcept = default;
  constexpr explicit ActionId(std::uint16_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr std::uint16_t value() const noexcept { return value_; }

  friend constexpr bool operator==(ActionId left, ActionId right) noexcept {
    return left.value_ == right.value_;
  }
  friend constexpr bool operator!=(ActionId left, ActionId right) noexcept {
    return !(left == right);
  }

private:
  std::uint16_t value_{0};
};

// Activate/Deactivate carry a state rule's level; Trigger is an event rule's
// one-shot edge.
enum class ActionCommandKind : std::uint8_t { Activate, Deactivate, Trigger };

struct ActionCommand {
  ActionId action{};
  ActionCommandKind kind{ActionCommandKind::Deactivate};

  friend constexpr bool operator==(const ActionCommand &left, const ActionCommand &right) noexcept {
    return left.action == right.action && left.kind == right.kind;
  }
  friend constexpr bool operator!=(const ActionCommand &left, const ActionCommand &right) noexcept {
    return !(left == right);
  }
};

// Output port. Every registered sink receives every command; a sink ignores
// ActionIds it does not handle. execute() runs on the provider's dispatcher
// context: it must not block, must not call back into the engine or provider,
// and owns any bounded queueing or overflow policy of its own. The engine
// does not own sinks; the destructor is protected and non-virtual.
class ActionSink {
public:
  virtual void execute(const ActionCommand &command) noexcept = 0;

protected:
  ActionSink() noexcept = default;
  ActionSink(const ActionSink &) noexcept = default;
  ActionSink &operator=(const ActionSink &) noexcept = default;
  ~ActionSink() = default;
};

} // namespace action_engine
