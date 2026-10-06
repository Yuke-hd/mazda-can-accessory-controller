#include "companion_protocol/active_document.hpp"

#include "companion_protocol/crc32.hpp"

namespace companion_protocol {

std::optional<ActiveDocument> ActiveDocument::selected(ConfigSource source,
                                                       ByteView canonical) noexcept {
  if (source == ConfigSource::None || canonical.size() > 0xFFFFU) {
    return std::nullopt;
  }
  ActiveDocument document{};
  document.source_ = source;
  document.bytes_ = canonical;
  document.crc32_ = companion_protocol::crc32(canonical);
  return document;
}

} // namespace companion_protocol
