#include "companion_protocol/output_catalog_export.hpp"

#include <algorithm>

namespace companion_protocol {

std::optional<EncodedOutputCatalogPage>
OutputCatalogExport::encode_page(const std::uint32_t token,
                                 const std::uint16_t offset) const noexcept {
  if (!available() || token == 0U || offset > document_.size())
    return std::nullopt;

  const auto length =
      static_cast<std::uint16_t>(std::min(document_.size() - offset, kOutputCatalogPageDataBytes));
  EncodedOutputCatalogPage page{};
  if (!page.push(kOutputCatalogFormat) || !page.push_u32(token) ||
      !page.push_u16(document_length()) || !page.push_u16(offset) || !page.push_u16(length) ||
      !page.append(ByteView{document_.data() + offset, length}))
    return std::nullopt;
  return page;
}

} // namespace companion_protocol
