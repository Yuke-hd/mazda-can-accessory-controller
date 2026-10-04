#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "companion_protocol/active_document.hpp"
#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

inline constexpr std::size_t kReadBackHeaderBytes = 9;
inline constexpr std::size_t kMaxReadBackDataBytes = 200;
inline constexpr std::size_t kMaxReadBackPageBytes = kReadBackHeaderBytes + kMaxReadBackDataBytes;

using EncodedReadBackPage = BoundedBytes<kMaxReadBackPageBytes>;

// Encodes the read-back page at `page_offset`: the 9-byte header and up to
// 200 data bytes. An offset equal to the length gives a page with no data;
// a larger offset gives std::nullopt.
[[nodiscard]] std::optional<EncodedReadBackPage>
encode_read_back_page(const ActiveDocument &document, std::uint16_t page_offset) noexcept;

} // namespace companion_protocol
