#pragma once

namespace companion_protocol {

// The outcome of HookGuard::ensure().
enum class HookState : unsigned char {
  // The guard was already in the slot.
  AlreadyInstalled,
  // The slot held another function. The guard replaced it and now forwards to
  // it.
  Installed,
  // The slot is empty, or the guard has nothing to forward to. Treat the
  // guarded operation as unavailable.
  Missing,
};

// Keeps a guard function installed in a hook slot that the host stack owns and
// may overwrite. NimBLE, for example, re-initializes its bond store callbacks
// at every host sync. ensure() is idempotent: call it whenever the host may
// have reset the slot. On each re-install it records the host's current
// function, which the guard then forwards to. `Hook` is a function pointer
// type.
template <typename Hook> class HookGuard {
public:
  explicit constexpr HookGuard(const Hook guard) noexcept : guard_{guard} {}

  [[nodiscard]] HookState ensure(Hook &slot) noexcept {
    if (slot == guard_)
      return original_ != nullptr ? HookState::AlreadyInstalled : HookState::Missing;
    if (slot == nullptr)
      return HookState::Missing;
    original_ = slot;
    slot = guard_;
    return HookState::Installed;
  }

  // The host function the guard forwards to, or nullptr before installation.
  [[nodiscard]] Hook original() const noexcept { return original_; }

private:
  Hook guard_;
  Hook original_{nullptr};
};

} // namespace companion_protocol
