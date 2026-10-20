#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/output_catalog_export.hpp"
#include "companion_protocol/output_catalog_selector.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace {
using namespace companion_protocol;
constexpr std::uint32_t kToken = 0x01020304;

std::array<std::uint8_t, 7> selector(std::uint16_t offset, std::uint32_t token = kToken,
                                     std::uint8_t format = kOutputCatalogFormat) {
  return {format,
          static_cast<std::uint8_t>(token),
          static_cast<std::uint8_t>(token >> 8U),
          static_cast<std::uint8_t>(token >> 16U),
          static_cast<std::uint8_t>(token >> 24U),
          static_cast<std::uint8_t>(offset),
          static_cast<std::uint8_t>(offset >> 8U)};
}

std::vector<std::uint8_t> bytes(const EncodedOutputCatalogPage &page) {
  return {page.view().begin(), page.view().end()};
}
} // namespace

TEST_CASE("static output catalog pages match reference headers and contiguous payloads") {
  std::array<std::uint8_t, 1518> document{};
  for (std::size_t i = 0; i < document.size(); ++i)
    document[i] = static_cast<std::uint8_t>(i);
  const OutputCatalogExport catalog{document};
  REQUIRE(catalog.available());
  CHECK(catalog.document_length() == 1518);
  const std::array<std::uint8_t, 11> first_header{1, 4, 3, 2, 1, 0xee, 5, 0, 0, 0xc8, 0};
  const auto first = catalog.encode_page(kToken, 0);
  REQUIRE(first.has_value());
  CHECK(first->size() == 211);
  for (std::size_t i = 0; i < first_header.size(); ++i)
    CHECK((*first)[i] == first_header[i]);

  for (const std::uint16_t offset : {0, 200, 1400, 1518}) {
    const auto page = catalog.encode_page(kToken, offset);
    REQUIRE(page.has_value());
    const auto length = std::min<std::size_t>(200, document.size() - offset);
    CHECK(page->size() == 11 + length);
    CHECK((*page)[7] == static_cast<std::uint8_t>(offset));
    CHECK((*page)[8] == static_cast<std::uint8_t>(offset >> 8U));
    CHECK((*page)[9] == length);
    CHECK((*page)[10] == 0);
    for (std::size_t i = 0; i < length; ++i)
      CHECK((*page)[11 + i] == document[offset + i]);
  }
  CHECK_FALSE(catalog.encode_page(kToken, 1519).has_value());
  CHECK_FALSE(catalog.encode_page(0, 0).has_value());
}

TEST_CASE("output selector validates in wire order without changing the selected page") {
  std::array<std::uint8_t, 1518> document{};
  const OutputCatalogExport catalog{document};
  OutputCatalogSelector selected{catalog};
  selected.connect(7, kToken);
  REQUIRE(selected.select(7, selector(200)) == AttError::None);
  const auto before = selected.read(7);
  REQUIRE(before.has_value());
  const auto unchanged = [&] {
    const auto after = selected.read(7);
    REQUIRE(after.has_value());
    CHECK(bytes(*after) == bytes(*before));
  };

  const auto invalid = selector(65535, 0, 2);
  CHECK(selected.select(7, ByteView{invalid.data(), 6}) == AttError::InvalidAttributeValueLength);
  unchanged();
  std::array<std::uint8_t, 8> too_long{};
  CHECK(selected.select(7, too_long) == AttError::InvalidAttributeValueLength);
  unchanged();
  CHECK(selected.select(7, invalid) == AttError::UnsupportedOperation);
  unchanged();
  CHECK(selected.select(7, selector(65535, 0)) == AttError::InvalidState);
  unchanged();
  CHECK(selected.select(7, selector(1519)) == AttError::InvalidPdu);
  unchanged();
  CHECK(selected.select(8, selector(0)) == AttError::InvalidState);
  unchanged();
  CHECK(selected.select(7, selector(1518)) == AttError::None);
  REQUIRE(selected.read(7).has_value());
  CHECK(selected.read(7)->size() == 11);
}

TEST_CASE("output selection resets on reconnect and rejects previous connection tokens") {
  std::array<std::uint8_t, 201> document{};
  const OutputCatalogExport catalog{document};
  OutputCatalogSelector selected{catalog};
  CHECK_FALSE(selected.read(7).has_value());
  selected.connect(7, 0);
  CHECK(selected.token_for_connection(7) == 0);
  selected.connect(7, kToken);
  CHECK(selected.select(7, selector(200)) == AttError::None);
  selected.connect(8, kToken + 1);
  CHECK_FALSE(selected.read(8).has_value());
  selected.disconnect(8);
  CHECK(selected.token_for_connection(7) == kToken);
  selected.disconnect(7);
  CHECK_FALSE(selected.read(7).has_value());
  selected.connect(7, kToken + 1);
  CHECK(selected.select(7, selector(200)) == AttError::InvalidState);
  const auto page = selected.read(7);
  REQUIRE(page.has_value());
  CHECK((*page)[1] == 5);
  CHECK((*page)[7] == 0);
  CHECK((*page)[8] == 0);
  CHECK(selected.select(7, selector(200, kToken + 1)) == AttError::None);
  selected.bind(catalog);
  CHECK_FALSE(selected.read(7).has_value());
}

TEST_CASE("output catalog size bounds fail closed and allow the maximum document") {
  std::array<std::uint8_t, kMaxOutputCatalogDocumentBytes + 1> document{};
  const OutputCatalogExport maximum{ByteView{document.data(), kMaxOutputCatalogDocumentBytes}};
  CHECK(maximum.available());
  CHECK(maximum.document_length() == 16384);
  REQUIRE(maximum.encode_page(kToken, 16384).has_value());
  CHECK(maximum.encode_page(kToken, 16384)->size() == 11);
  const OutputCatalogExport empty{ByteView{}};
  const OutputCatalogExport oversized{document};
  for (const auto *catalog : {&empty, &oversized}) {
    CHECK_FALSE(catalog->available());
    CHECK(catalog->document_length() == 0);
    CHECK_FALSE(catalog->encode_page(kToken, 0).has_value());
    OutputCatalogSelector selected{*catalog};
    selected.connect(7, kToken);
    CHECK(selected.select(7, selector(65535, 0, 2)) == AttError::UnsupportedOperation);
    CHECK(selected.select(7, selector(65535)) == AttError::InvalidState);
    CHECK_FALSE(selected.read(7).has_value());
    CHECK(selected.token_for_connection(7) == 0);
  }
  OutputCatalogSelector unbound;
  unbound.connect(7, kToken);
  CHECK(unbound.select(7, selector(0)) == AttError::InvalidState);
  CHECK_FALSE(unbound.read(7).has_value());
}
