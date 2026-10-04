#include "companion_protocol/read_back.hpp"

#include <algorithm>

namespace companion_protocol {

std::optional<EncodedReadBackPage> encode_read_back_page(const ActiveDocument &document,
                                                         std::uint16_t page_offset) noexcept {
  if (page_offset > document.length()) {
    return std::nullopt;
  }
  EncodedReadBackPage page{};
  page.push(static_cast<std::uint8_t>(document.source()));
  page.push_u16(document.length());
  page.push_u32(document.crc32());
  page.push_u16(page_offset);
  const ByteView rest = document.bytes().from(page_offset);
  const std::size_t count = std::min(rest.size(), kMaxReadBackDataBytes);
  page.append(ByteView{rest.data(), count});
  return page;
}

} // namespace companion_protocol
