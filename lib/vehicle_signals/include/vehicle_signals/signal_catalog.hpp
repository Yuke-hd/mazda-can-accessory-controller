#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_signals {

// One named raw value of an Enum signal. Keys are persistent configuration
// identities and must be unique within their signal.
struct SignalEnumChoice {
  std::uint16_t value{0};
  std::string_view key{};
};

// Static description of one signal. All referenced storage (key text and the
// choices array) must have static lifetime; metadata never owns memory.
// `key` is the persistent configuration identity; `id` is a per-build runtime
// handle. Enum signals list their choices; other types have none.
struct SignalMetadata {
  SignalId id{};
  std::string_view key{};
  SignalType type{SignalType::Boolean};
  SignalUnit unit{SignalUnit::None};
  ValidationStatus validation{ValidationStatus::Reference};
  SignalCapabilities capabilities{};
  const SignalEnumChoice *choices{nullptr};
  std::size_t choice_count{0};

  [[nodiscard]] constexpr const SignalEnumChoice *find_choice(std::uint16_t value) const noexcept {
    for (std::size_t index = 0; choices != nullptr && index < choice_count; ++index) {
      if (choices[index].value == value) {
        return &choices[index];
      }
    }
    return nullptr;
  }
  [[nodiscard]] constexpr const SignalEnumChoice *
  find_choice(std::string_view choice_key) const noexcept {
    for (std::size_t index = 0; choices != nullptr && index < choice_count; ++index) {
      if (choices[index].key == choice_key) {
        return &choices[index];
      }
    }
    return nullptr;
  }

  // True when `value` has this signal's type and, for Enum, names a choice.
  [[nodiscard]] constexpr bool accepts(const SignalValue &value) const noexcept {
    if (value.type() != type) {
      return false;
    }
    if (type == SignalType::Enum) {
      return find_choice(*value.as_enumeration()) != nullptr;
    }
    return true;
  }
};

// Non-owning view over a fixed, statically stored metadata array. Lookups are
// linear and allocation-free; they return nullptr for an invalid id (including
// zero), an unknown id, or a missing key.
class SignalCatalogView final {
public:
  constexpr SignalCatalogView() noexcept = default;
  constexpr SignalCatalogView(const SignalMetadata *entries, std::size_t count) noexcept
      : entries_(entries), count_(entries == nullptr ? 0 : count) {}
  template <std::size_t N>
  constexpr SignalCatalogView(const SignalMetadata (&entries)[N]) noexcept // NOLINT: implicit
      : entries_(entries), count_(N) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] constexpr const SignalMetadata *begin() const noexcept { return entries_; }
  [[nodiscard]] constexpr const SignalMetadata *end() const noexcept {
    return entries_ == nullptr ? nullptr : entries_ + count_;
  }

  // Bounds-checked index access; nullptr when out of range.
  [[nodiscard]] constexpr const SignalMetadata *at(std::size_t index) const noexcept {
    return index < count_ ? &entries_[index] : nullptr;
  }

  [[nodiscard]] constexpr const SignalMetadata *find(SignalId id) const noexcept {
    if (!id.valid()) {
      return nullptr;
    }
    for (std::size_t index = 0; index < count_; ++index) {
      if (entries_[index].id == id) {
        return &entries_[index];
      }
    }
    return nullptr;
  }
  [[nodiscard]] constexpr const SignalMetadata *find(std::string_view key) const noexcept {
    if (key.empty()) {
      return nullptr;
    }
    for (std::size_t index = 0; index < count_; ++index) {
      if (entries_[index].key == key) {
        return &entries_[index];
      }
    }
    return nullptr;
  }

  // Structural check intended for static_assert on a concrete catalog: ids are
  // valid and unique, keys are non-empty and unique, every signal has at least
  // one capability, Enum signals have uniquely valued and keyed choices, other
  // types have no choices, and only Number signals carry a unit.
  [[nodiscard]] constexpr bool well_formed() const noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
      const SignalMetadata &entry = entries_[index];
      if (!entry.id.valid() || entry.key.empty() || entry.capabilities.empty()) {
        return false;
      }
      if (entry.type != SignalType::Number && entry.unit != SignalUnit::None) {
        return false;
      }
      if (!choices_well_formed(entry)) {
        return false;
      }
      for (std::size_t other = index + 1; other < count_; ++other) {
        if (entries_[other].id == entry.id || entries_[other].key == entry.key) {
          return false;
        }
      }
    }
    return true;
  }

private:
  [[nodiscard]] static constexpr bool choices_well_formed(const SignalMetadata &entry) noexcept {
    if (entry.type != SignalType::Enum) {
      return entry.choices == nullptr && entry.choice_count == 0;
    }
    if (entry.choices == nullptr || entry.choice_count == 0) {
      return false;
    }
    for (std::size_t index = 0; index < entry.choice_count; ++index) {
      if (entry.choices[index].key.empty()) {
        return false;
      }
      for (std::size_t other = index + 1; other < entry.choice_count; ++other) {
        if (entry.choices[other].value == entry.choices[index].value ||
            entry.choices[other].key == entry.choices[index].key) {
          return false;
        }
      }
    }
    return true;
  }

  const SignalMetadata *entries_{nullptr};
  std::size_t count_{0};
};

} // namespace vehicle_signals
