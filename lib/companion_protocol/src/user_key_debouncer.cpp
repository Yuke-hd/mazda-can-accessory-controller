#include "companion_protocol/user_key_debouncer.hpp"

namespace companion_protocol {

bool UserKeyDebouncer::sample(const UserKeyLevel level) noexcept {
  if (level == stable_) {
    changed_samples_ = 0U;
    return false;
  }
  ++changed_samples_;
  if (changed_samples_ < kUserKeyDebounceSamples)
    return false;
  stable_ = level;
  changed_samples_ = 0U;
  return stable_ == UserKeyLevel::Pressed;
}

} // namespace companion_protocol
