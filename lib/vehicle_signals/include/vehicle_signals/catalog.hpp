#pragma once

#include <cstddef>
#include <string_view>

#include "vehicle_signals/types.hpp"

namespace vehicle_signals {

// A read-only, non-owning catalog view. The metadata array, text, and choice
// arrays must remain alive and unchanged for the lifetime of this view.
// SignalMetadata::name is the canonical key used by find(key).
class SignalCatalogView final {
public:
  constexpr SignalCatalogView() noexcept = default;

  constexpr SignalCatalogView(const SignalMetadata *signals, std::size_t count) noexcept
      : signals_(signals), count_(count) {}

  [[nodiscard]] constexpr const SignalMetadata *data() const noexcept { return signals_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

  [[nodiscard]] constexpr const SignalMetadata *begin() const noexcept { return signals_; }
  [[nodiscard]] constexpr const SignalMetadata *end() const noexcept {
    return signals_ == nullptr ? nullptr : signals_ + count_;
  }

  [[nodiscard]] constexpr const SignalMetadata *find(SignalId id) const noexcept {
    if (!id.valid() || signals_ == nullptr) {
      return nullptr;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      if (signals_[i].id == id) {
        return &signals_[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] constexpr const SignalMetadata *find(std::string_view key) const noexcept {
    if (key.empty() || signals_ == nullptr) {
      return nullptr;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      if (signals_[i].name == key) {
        return &signals_[i];
      }
    }
    return nullptr;
  }

private:
  const SignalMetadata *signals_{nullptr};
  std::size_t count_{0};
};

} // namespace vehicle_signals
