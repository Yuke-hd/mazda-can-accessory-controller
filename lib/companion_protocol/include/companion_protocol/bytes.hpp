#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace companion_protocol {

// Non-owning, read-only view of a byte sequence, such as one ATT write value.
// The viewed storage must outlive the view.
class ByteView final {
public:
  constexpr ByteView() noexcept = default;
  constexpr ByteView(const std::uint8_t *data, std::size_t size) noexcept
      : data_(size == 0 ? nullptr : data), size_(data == nullptr ? 0 : size) {}
  template <std::size_t N>
  constexpr ByteView(const std::array<std::uint8_t, N> &bytes) noexcept // NOLINT: implicit
      : data_(N == 0 ? nullptr : bytes.data()), size_(N) {}

  [[nodiscard]] constexpr const std::uint8_t *data() const noexcept { return data_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] constexpr const std::uint8_t *begin() const noexcept { return data_; }
  [[nodiscard]] constexpr const std::uint8_t *end() const noexcept {
    return data_ == nullptr ? nullptr : data_ + size_;
  }
  // Unchecked element access; the caller keeps `index` below size().
  [[nodiscard]] constexpr std::uint8_t operator[](std::size_t index) const noexcept {
    return data_[index];
  }
  // The bytes from `offset` to the end; empty when `offset` is past the end.
  [[nodiscard]] constexpr ByteView from(std::size_t offset) const noexcept {
    return offset >= size_ ? ByteView{} : ByteView{data_ + offset, size_ - offset};
  }

private:
  const std::uint8_t *data_{nullptr};
  std::size_t size_{0};
};

// A fixed-capacity encoded value: at most `Capacity` bytes, of which size()
// are meaningful. Encoders build these without heap allocation.
template <std::size_t Capacity> class BoundedBytes final {
public:
  static constexpr std::size_t kCapacity = Capacity;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr ByteView view() const noexcept { return ByteView{bytes_.data(), size_}; }
  [[nodiscard]] constexpr std::uint8_t operator[](std::size_t index) const noexcept {
    return bytes_[index];
  }

  // Appends one byte; false, with nothing written, when the value is full.
  constexpr bool push(std::uint8_t byte) noexcept {
    if (size_ == Capacity) {
      return false;
    }
    bytes_[size_++] = byte;
    return true;
  }
  constexpr bool push_u16(std::uint16_t value) noexcept {
    return room_for(2) && push(static_cast<std::uint8_t>(value & 0xFFU)) &&
           push(static_cast<std::uint8_t>(value >> 8U));
  }
  constexpr bool push_u32(std::uint32_t value) noexcept {
    return room_for(4) && push_u16(static_cast<std::uint16_t>(value & 0xFFFFU)) &&
           push_u16(static_cast<std::uint16_t>(value >> 16U));
  }
  // Appends every byte of `bytes`, or nothing when they do not all fit.
  constexpr bool append(ByteView bytes) noexcept {
    if (!room_for(bytes.size())) {
      return false;
    }
    for (const std::uint8_t byte : bytes) {
      bytes_[size_++] = byte;
    }
    return true;
  }

private:
  [[nodiscard]] constexpr bool room_for(std::size_t count) const noexcept {
    return Capacity - size_ >= count;
  }

  std::array<std::uint8_t, Capacity> bytes_{};
  std::size_t size_{0};
};

} // namespace companion_protocol
