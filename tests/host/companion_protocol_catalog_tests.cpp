#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/signal_catalog_export.hpp"
#include "companion_protocol/signal_catalog_selector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace companion_protocol;
using vehicle_signals::SignalCapabilities;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalCatalogView;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::ValidationStatus;

std::uint16_t u16_at(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes.at(offset) |
                                    (static_cast<std::uint16_t>(bytes.at(offset + 1)) << 8U));
}

std::uint16_t u16_at(const ByteView bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] |
                                    (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

void append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

std::array<std::uint8_t, kSignalCatalogSelectorBytes>
selector_bytes(std::uint8_t format, std::uint32_t token, std::uint16_t offset) {
  return {format,
          static_cast<std::uint8_t>(token & 0xFFU),
          static_cast<std::uint8_t>((token >> 8U) & 0xFFU),
          static_cast<std::uint8_t>((token >> 16U) & 0xFFU),
          static_cast<std::uint8_t>(token >> 24U),
          static_cast<std::uint8_t>(offset & 0xFFU),
          static_cast<std::uint8_t>(offset >> 8U)};
}

std::uint32_t u32_at(const ByteView bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(u16_at(bytes, offset)) |
         (static_cast<std::uint32_t>(u16_at(bytes, offset + 2)) << 16U);
}

struct ExportedChoice {
  std::uint16_t handle{0};
  std::string key{};
};

struct ExportedSignal {
  std::uint16_t handle{0};
  std::string key{};
  std::uint8_t type{0};
  std::string unit{};
  std::string evidence{};
  std::vector<ExportedChoice> choices{};
};

std::optional<std::vector<ExportedSignal>> decode_catalog(const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() < 3 || bytes[0] != kSignalCatalogFormat) {
    return std::nullopt;
  }
  const std::size_t signal_count = u16_at(bytes, 1);
  std::size_t cursor = 3;
  std::vector<ExportedSignal> signals{};
  signals.reserve(signal_count);

  for (std::size_t index = 0; index < signal_count; ++index) {
    if (cursor + 2 > bytes.size()) {
      return std::nullopt;
    }
    const std::size_t body_length = u16_at(bytes, cursor);
    cursor += 2;
    const std::size_t body_end = cursor + body_length;
    if (body_end > bytes.size() || body_length < 2) {
      return std::nullopt;
    }

    ExportedSignal signal{};
    signal.handle = u16_at(bytes, cursor);
    cursor += 2;
    std::array<bool, 256> seen_tags{};
    while (cursor < body_end) {
      if (cursor + 3 > body_end) {
        return std::nullopt;
      }
      const std::uint8_t tag = bytes[cursor++];
      if (seen_tags[tag]) {
        return std::nullopt;
      }
      seen_tags[tag] = true;
      const std::size_t value_length = u16_at(bytes, cursor);
      cursor += 2;
      const std::size_t value_end = cursor + value_length;
      if (value_end > body_end) {
        return std::nullopt;
      }
      const auto text = [&bytes, cursor, value_length] {
        return std::string(reinterpret_cast<const char *>(bytes.data() + cursor), value_length);
      };

      switch (tag) {
      case 1:
        signal.key = text();
        break;
      case 2:
        if (value_length != 1) {
          return std::nullopt;
        }
        signal.type = bytes[cursor];
        break;
      case 3:
        signal.unit = text();
        break;
      case 4:
        signal.evidence = text();
        break;
      case 5: {
        if (value_length < 2) {
          return std::nullopt;
        }
        const std::size_t choice_count = u16_at(bytes, cursor);
        std::size_t choice_cursor = cursor + 2;
        for (std::size_t choice_index = 0; choice_index < choice_count; ++choice_index) {
          if (choice_cursor + 3 > value_end) {
            return std::nullopt;
          }
          ExportedChoice choice{};
          choice.handle = u16_at(bytes, choice_cursor);
          choice_cursor += 2;
          const std::size_t key_length = bytes[choice_cursor++];
          if (choice_cursor + key_length > value_end) {
            return std::nullopt;
          }
          choice.key =
              std::string(reinterpret_cast<const char *>(bytes.data() + choice_cursor), key_length);
          choice_cursor += key_length;
          signal.choices.push_back(std::move(choice));
        }
        if (choice_cursor != value_end) {
          return std::nullopt;
        }
        break;
      }
      default:
        break;
      }
      cursor = value_end;
    }
    if (cursor != body_end) {
      return std::nullopt;
    }
    if (!seen_tags[1] || !seen_tags[2] || !seen_tags[3] || !seen_tags[4] || signal.key.empty() ||
        signal.unit.empty() || signal.evidence.empty() || signal.type < 1 || signal.type > 3 ||
        (signal.type == 3) != seen_tags[5]) {
      return std::nullopt;
    }
    signals.push_back(std::move(signal));
  }
  if (cursor != bytes.size()) {
    return std::nullopt;
  }
  return signals;
}

std::vector<std::uint8_t> single_signal_document(std::uint8_t type, std::uint8_t omitted_tag = 0,
                                                 std::uint8_t duplicate_tag = 0,
                                                 bool include_choices = false) {
  std::vector<std::uint8_t> body{1, 0};
  const auto append_tlv = [&body](std::uint8_t tag, const std::vector<std::uint8_t> &value) {
    body.push_back(tag);
    append_u16(body, static_cast<std::uint16_t>(value.size()));
    body.insert(body.end(), value.begin(), value.end());
  };
  const auto append_text = [&append_tlv](std::uint8_t tag, std::string_view text) {
    append_tlv(tag, std::vector<std::uint8_t>(text.begin(), text.end()));
  };

  if (omitted_tag != 1) {
    append_text(1, "signal.test");
  }
  if (omitted_tag != 2) {
    append_tlv(2, {type});
  }
  if (omitted_tag != 3) {
    append_text(3, "none");
  }
  if (omitted_tag != 4) {
    append_text(4, "reference");
  }
  if (include_choices) {
    append_tlv(5, {1, 0, 1, 0, 1, 'a'});
  }
  if (duplicate_tag != 0) {
    append_text(duplicate_tag, "duplicate");
  }

  std::vector<std::uint8_t> document{kSignalCatalogFormat, 1, 0};
  append_u16(document, static_cast<std::uint16_t>(body.size()));
  document.insert(document.end(), body.begin(), body.end());
  return document;
}

std::optional<std::vector<std::uint8_t>> download_document(const SignalCatalogExport &catalog,
                                                           std::uint32_t token) {
  if (!catalog.available()) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> document{};
  std::uint16_t offset = 0;
  while (true) {
    const auto page = catalog.encode_page(token, offset);
    if (!page.has_value()) {
      return std::nullopt;
    }
    const ByteView bytes = page->view();
    if (bytes.size() < kSignalCatalogPageHeaderBytes || bytes[0] != kSignalCatalogFormat ||
        u32_at(bytes, 1) != token || u16_at(bytes, 5) != catalog.document_length() ||
        u16_at(bytes, 7) != offset) {
      return std::nullopt;
    }
    const std::size_t data_length = u16_at(bytes, 9);
    if (data_length > kSignalCatalogPageDataBytes || bytes.size() != 11 + data_length) {
      return std::nullopt;
    }
    document.insert(document.end(), bytes.begin() + kSignalCatalogPageHeaderBytes, bytes.end());
    if (offset + data_length == catalog.document_length()) {
      break;
    }
    if (data_length == 0 || offset + data_length > catalog.document_length()) {
      return std::nullopt;
    }
    offset = static_cast<std::uint16_t>(offset + data_length);
  }
  return document;
}

SignalMetadata boolean_signal(std::uint16_t id, std::string_view key,
                              ValidationStatus validation = ValidationStatus::Reference,
                              SignalCapabilities capabilities = SignalCapability::Read) {
  return {SignalId{id}, key, SignalType::Boolean, SignalUnit::None, validation, capabilities,
          nullptr,      0};
}

std::string fixed_key(std::string prefix, std::size_t length, char fill = 'x') {
  if (prefix.size() > length) {
    prefix.resize(length);
  }
  prefix.append(length - prefix.size(), fill);
  return prefix;
}

bool has_subsequence(const std::vector<std::uint8_t> &bytes, std::array<std::uint8_t, 2> sequence) {
  for (std::size_t index = 0; index + 1 < bytes.size(); ++index) {
    if (bytes[index] == sequence[0] && bytes[index + 1] == sequence[1]) {
      return true;
    }
  }
  return false;
}

std::vector<std::uint8_t> page_bytes(const EncodedSignalCatalogPage &page) {
  const ByteView view = page.view();
  return {view.begin(), view.end()};
}

} // namespace

TEST_CASE("catalog document decoder enforces mandatory tags and enum choice shape") {
  for (std::uint8_t tag = 1; tag <= 4; ++tag) {
    CHECK_FALSE(decode_catalog(single_signal_document(1, tag)).has_value());
  }
  CHECK_FALSE(decode_catalog(single_signal_document(1, 0, 1)).has_value());
  CHECK_FALSE(decode_catalog(single_signal_document(3, 0, 0, false)).has_value());
  CHECK_FALSE(decode_catalog(single_signal_document(1, 0, 0, true)).has_value());
  CHECK_FALSE(decode_catalog(single_signal_document(2, 0, 0, true)).has_value());

  CHECK(decode_catalog(single_signal_document(1)).has_value());
  CHECK(decode_catalog(single_signal_document(2)).has_value());
  CHECK(decode_catalog(single_signal_document(3, 0, 0, true)).has_value());
  CHECK_FALSE(decode_catalog(single_signal_document(0x7F)).has_value());
}

TEST_CASE("catalog export emits explicit type, unit, evidence, and ordered choice tokens") {
  const SignalEnumChoice choices[]{{0xA55A, "park"}, {0xB66B, "drive"}};
  const SignalMetadata rows[]{
      {SignalId{0xD123}, "vehicle.brake_pressed", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{0xE234}, "vehicle.speed", SignalType::Number, SignalUnit::KilometresPerHour,
       ValidationStatus::Observed, SignalCapability::Read, nullptr, 0},
      {SignalId{0xF345}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, choices, 2},
      {SignalId{0xF456}, "engine.rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{0xF567}, "vehicle.accel_x", SignalType::Number, SignalUnit::MetresPerSecondSquared,
       ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0},
      {SignalId{0xF678}, "vehicle.unknown_unit", SignalType::Number, static_cast<SignalUnit>(0xFE),
       static_cast<ValidationStatus>(0xFE), SignalCapability::Read, nullptr, 0},
      {SignalId{0xF789}, "notify.only", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Notify, nullptr, 0},
      {SignalId{0xF89A}, "unsupported.type", static_cast<SignalType>(0xFE), SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
  };
  const SignalCatalogExport catalog{SignalCatalogView{rows}};

  REQUIRE(catalog.available());
  CHECK(catalog.signal_count() == 6);
  const auto document = download_document(catalog, 0x12345678U);
  REQUIRE(document.has_value());
  const auto decoded = decode_catalog(*document);
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->size() == 6);

  CHECK((*decoded)[0].handle == 1);
  CHECK((*decoded)[0].key == "vehicle.brake_pressed");
  CHECK((*decoded)[0].type == 1);
  CHECK((*decoded)[0].unit == "none");
  CHECK((*decoded)[0].evidence == "reference");

  CHECK((*decoded)[1].handle == 2);
  CHECK((*decoded)[1].key == "vehicle.speed");
  CHECK((*decoded)[1].type == 2);
  CHECK((*decoded)[1].unit == "km/h");
  CHECK((*decoded)[1].evidence == "observed");

  CHECK((*decoded)[2].handle == 3);
  CHECK((*decoded)[2].key == "vehicle.gear");
  CHECK((*decoded)[2].type == 3);
  CHECK((*decoded)[2].unit == "none");
  CHECK((*decoded)[2].evidence == "confirmed");
  REQUIRE((*decoded)[2].choices.size() == 2);
  CHECK((*decoded)[2].choices[0].handle == 1);
  CHECK((*decoded)[2].choices[0].key == "park");
  CHECK((*decoded)[2].choices[1].handle == 2);
  CHECK((*decoded)[2].choices[1].key == "drive");

  CHECK((*decoded)[3].unit == "rpm");
  CHECK((*decoded)[4].unit == "m/s^2");
  CHECK((*decoded)[5].unit == "unknown");
  CHECK((*decoded)[5].evidence == "unknown");
  for (const auto &signal : *decoded) {
    CHECK_FALSE(signal.unit.empty());
    CHECK(signal.unit.size() <= kMaxSignalCatalogTokenBytes);
    CHECK_FALSE(signal.evidence.empty());
    CHECK(signal.evidence.size() <= kMaxSignalCatalogTokenBytes);
  }
}

TEST_CASE("catalog emits none for normalized numeric signals") {
  const SignalMetadata rows[]{
      {SignalId{22}, "vehicle.accelerator_pedal_position", SignalType::Number, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{23}, "vehicle.brake_pressure", SignalType::Number, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, nullptr, 0}};
  const SignalCatalogExport catalog{SignalCatalogView{rows, 2}};
  const auto document = download_document(catalog, 0x12345678U);
  REQUIRE(document.has_value());
  const auto decoded = decode_catalog(*document);
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->size() == 2);
  CHECK((*decoded)[0].key == "vehicle.accelerator_pedal_position");
  CHECK((*decoded)[0].type == 2);
  CHECK((*decoded)[0].unit == "none");
  CHECK((*decoded)[0].evidence == "reference");
  CHECK((*decoded)[1].key == "vehicle.brake_pressure");
  CHECK((*decoded)[1].unit == "none");
  CHECK((*decoded)[1].evidence == "confirmed");
}

TEST_CASE("catalog handles follow provider order while runtime ids and enum values stay local") {
  const SignalEnumChoice choices_a[]{{0xBEEF, "park"}, {0xA55A, "drive"}};
  const SignalEnumChoice choices_b[]{{0xCAFE, "park"}, {0xCCDD, "drive"}};
  const SignalEnumChoice choices_added[]{{0xCAFE, "park"}, {0xCCDD, "drive"}, {0xDEAD, "sport"}};
  const SignalMetadata first_rows[]{
      {SignalId{0xD123}, "vehicle.door_open", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{0xE234}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, choices_a, 2},
  };
  const SignalMetadata renumbered_rows[]{
      {SignalId{0x1287}, "vehicle.door_open", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{0x2398}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, choices_b, 2},
  };
  const SignalMetadata added_rows[]{
      {SignalId{0x34A9}, "vehicle.new_signal", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Observed, SignalCapability::Read, nullptr, 0},
      {SignalId{0x45BA}, "vehicle.door_open", SignalType::Boolean, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
      {SignalId{0x56CB}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, choices_added, 3},
  };

  const SignalCatalogExport first{SignalCatalogView{first_rows}};
  const SignalCatalogExport renumbered{SignalCatalogView{renumbered_rows}};
  const SignalCatalogExport added{SignalCatalogView{added_rows}};
  REQUIRE(first.available());
  REQUIRE(renumbered.available());
  REQUIRE(added.available());
  const auto first_document = download_document(first, 1);
  const auto renumbered_document = download_document(renumbered, 1);
  REQUIRE(first_document.has_value());
  REQUIRE(renumbered_document.has_value());
  CHECK(*first_document == *renumbered_document);

  const auto added_document = download_document(added, 1);
  REQUIRE(added_document.has_value());
  const auto added_decoded = decode_catalog(*added_document);
  REQUIRE(added_decoded.has_value());
  REQUIRE(added_decoded->size() == 3);
  CHECK((*added_decoded)[0].key == "vehicle.new_signal");
  CHECK((*added_decoded)[0].handle == 1);
  CHECK((*added_decoded)[1].key == "vehicle.door_open");
  CHECK((*added_decoded)[1].handle == 2);
  CHECK((*added_decoded)[2].key == "vehicle.gear");
  CHECK((*added_decoded)[2].handle == 3);
  REQUIRE((*added_decoded)[2].choices.size() == 3);
  CHECK((*added_decoded)[2].choices[0].key == "park");
  CHECK((*added_decoded)[2].choices[0].handle == 1);
  CHECK((*added_decoded)[2].choices[1].key == "drive");
  CHECK((*added_decoded)[2].choices[1].handle == 2);
  CHECK((*added_decoded)[2].choices[2].key == "sport");
  CHECK((*added_decoded)[2].choices[2].handle == 3);

  CHECK(first.provider_id_for_handle(1) == SignalId{0xD123});
  CHECK(first.provider_id_for_handle(2) == SignalId{0xE234});
  CHECK_FALSE(first.provider_id_for_handle(0).valid());
  CHECK_FALSE(first.provider_id_for_handle(3).valid());
  CHECK(first.wire_choice_handle(2, 0xBEEF) == 1);
  CHECK(first.wire_choice_handle(2, 0xA55A) == 2);
  CHECK(added.wire_choice_handle(3, 0xDEAD) == 3);
  CHECK_FALSE(first.wire_choice_handle(2, 0x7777).has_value());
  CHECK_FALSE(first.wire_choice_handle(1, 0xBEEF).has_value());
  CHECK_FALSE(first.wire_choice_handle(0, 0xBEEF).has_value());
  CHECK_FALSE(first.encode_page(0, 0).has_value());

  REQUIRE_FALSE(has_subsequence(*first_document, {0x23, 0xD1}));
  CHECK_FALSE(has_subsequence(*first_document, {0x34, 0xE2}));
  CHECK_FALSE(has_subsequence(*first_document, {0xEF, 0xBE}));
  CHECK_FALSE(has_subsequence(*first_document, {0x5A, 0xA5}));
}

TEST_CASE("empty catalog is available and exports a valid three-byte document") {
  const SignalCatalogExport catalog{SignalCatalogView{}};
  REQUIRE(catalog.available());
  CHECK(catalog.signal_count() == 0);
  CHECK(catalog.document_length() == 3);
  const auto document = download_document(catalog, 0xA1B2C3D4U);
  REQUIRE(document.has_value());
  CHECK(*document == std::vector<std::uint8_t>{1, 0, 0});
  const auto decoded = decode_catalog(*document);
  REQUIRE(decoded.has_value());
  CHECK(decoded->empty());
}

TEST_CASE("catalog selector applies framing and token error precedence") {
  const SignalMetadata rows[]{boolean_signal(1, "vehicle.test")};
  const SignalCatalogExport catalog{SignalCatalogView{rows}};
  REQUIRE(catalog.available());

  SignalCatalogSelector selector{catalog};
  constexpr std::uint32_t connection = 7U;
  constexpr std::uint32_t token = 0x10203040U;
  selector.connect(connection, token);

  const auto valid = selector_bytes(kSignalCatalogFormat, token, catalog.document_length());
  CHECK(selector.select(connection, valid) == AttError::None);
  const auto selected = selector.read(connection);
  REQUIRE(selected.has_value());
  CHECK(u16_at(selected->view(), 7) == catalog.document_length());

  const std::array<std::uint8_t, kSignalCatalogSelectorBytes - 1> short_selector{};
  CHECK(selector.select(connection, short_selector) == AttError::InvalidAttributeValueLength);

  const auto unknown_format = selector_bytes(0x7FU, token, 0xFFFFU);
  CHECK(selector.select(connection, unknown_format) == AttError::UnsupportedOperation);

  const auto wrong_token = selector_bytes(kSignalCatalogFormat, token + 1U, 0xFFFFU);
  CHECK(selector.select(connection, wrong_token) == AttError::InvalidState);

  const auto invalid_offset = selector_bytes(
      kSignalCatalogFormat, token, static_cast<std::uint16_t>(catalog.document_length() + 1U));
  CHECK(selector.select(connection, invalid_offset) == AttError::InvalidPdu);
}

TEST_CASE("catalog selector keeps a failed write from changing the selected page") {
  const SignalMetadata rows[]{boolean_signal(1, "vehicle.test")};
  const SignalCatalogExport catalog{SignalCatalogView{rows}};
  REQUIRE(catalog.available());

  SignalCatalogSelector selector{catalog};
  constexpr std::uint32_t connection = 7U;
  constexpr std::uint32_t token = 0x10203040U;
  selector.connect(connection, token);
  const std::uint16_t selected_offset = catalog.document_length();
  REQUIRE(selected_offset > 0U);
  CHECK(selector.select(connection, selector_bytes(kSignalCatalogFormat, token, selected_offset)) ==
        AttError::None);
  CHECK(selector.select(connection, selector_bytes(kSignalCatalogFormat, token,
                                                   static_cast<std::uint16_t>(
                                                       catalog.document_length() + 1U))) ==
        AttError::InvalidPdu);

  const auto selected = selector.read(connection);
  REQUIRE(selected.has_value());
  CHECK(u16_at(selected->view(), 7) == selected_offset);
  const auto selected_again = selector.read(connection);
  REQUIRE(selected_again.has_value());
  CHECK(page_bytes(*selected_again) == page_bytes(*selected));
}

TEST_CASE("catalog selector rejects an unavailable export") {
  const std::string oversized_key = fixed_key("signal", kMaxSignalCatalogKeyBytes + 1U);
  const SignalMetadata rows[]{boolean_signal(1, oversized_key)};
  const SignalCatalogExport catalog{SignalCatalogView{rows}};
  REQUIRE_FALSE(catalog.available());

  SignalCatalogSelector selector{catalog};
  constexpr std::uint32_t connection = 7U;
  constexpr std::uint32_t token = 0x10203040U;
  selector.connect(connection, token);
  CHECK(selector.token_for_connection(connection) == 0U);
  CHECK(selector.select(connection, selector_bytes(kSignalCatalogFormat, token, 0U)) ==
        AttError::InvalidState);
  CHECK_FALSE(selector.read(connection).has_value());
}

TEST_CASE("catalog selector isolates the connection and resets on disconnect") {
  const SignalMetadata rows[]{boolean_signal(1, "vehicle.test")};
  const SignalCatalogExport catalog{SignalCatalogView{rows}};
  REQUIRE(catalog.available());

  SignalCatalogSelector selector{catalog};
  constexpr std::uint32_t first_connection = 7U;
  constexpr std::uint32_t first_token = 0x10203040U;
  constexpr std::uint32_t second_connection = 8U;
  constexpr std::uint32_t second_token = 0x50607080U;
  selector.connect(first_connection, first_token);
  CHECK(selector.token_for_connection(first_connection) == first_token);
  CHECK(selector.token_for_connection(second_connection) == 0U);
  CHECK(selector.select(first_connection, selector_bytes(kSignalCatalogFormat, first_token,
                                                         catalog.document_length())) ==
        AttError::None);

  CHECK(selector.select(second_connection, selector_bytes(kSignalCatalogFormat, first_token, 0U)) ==
        AttError::InvalidState);
  CHECK_FALSE(selector.read(second_connection).has_value());
  selector.disconnect(second_connection);
  const auto first_page = selector.read(first_connection);
  REQUIRE(first_page.has_value());
  CHECK(u16_at(first_page->view(), 7) == catalog.document_length());

  selector.connect(second_connection, second_token);
  const auto still_first = selector.read(first_connection);
  REQUIRE(still_first.has_value());
  CHECK(u16_at(still_first->view(), 7) == catalog.document_length());

  selector.disconnect(first_connection);
  CHECK(selector.token_for_connection(first_connection) == 0U);
  CHECK_FALSE(selector.read(first_connection).has_value());
  selector.connect(first_connection, second_token);
  CHECK(selector.token_for_connection(first_connection) == second_token);
  CHECK(selector.select(first_connection, selector_bytes(kSignalCatalogFormat, first_token, 0U)) ==
        AttError::InvalidState);
  CHECK(selector.select(first_connection, selector_bytes(kSignalCatalogFormat, second_token, 0U)) ==
        AttError::None);
  const auto second_page = selector.read(first_connection);
  REQUIRE(second_page.has_value());
  CHECK(u16_at(second_page->view(), 7) == 0U);
}

TEST_CASE("catalog selector accepts a valid offset above one byte") {
  std::array<std::string, 16> keys{};
  std::array<SignalMetadata, 16> rows{};
  for (std::size_t index = 0; index < rows.size(); ++index) {
    keys[index] = "signal." + std::to_string(index);
    rows[index] = boolean_signal(static_cast<std::uint16_t>(index + 1U), keys[index]);
  }
  const SignalCatalogExport catalog{SignalCatalogView{rows.data(), rows.size()}};
  REQUIRE(catalog.available());
  REQUIRE(catalog.document_length() > 255U);

  SignalCatalogSelector selector{catalog};
  constexpr std::uint32_t connection = 7U;
  constexpr std::uint32_t token = 0x10203040U;
  constexpr std::uint16_t offset = 256U;
  REQUIRE(offset <= catalog.document_length());
  selector.connect(connection, token);
  CHECK(selector.select(connection, selector_bytes(kSignalCatalogFormat, token, offset)) ==
        AttError::None);
  const auto page = selector.read(connection);
  REQUIRE(page.has_value());
  CHECK(u16_at(page->view(), 7) == offset);
}

TEST_CASE("catalog pages use a little-endian header and bounded contiguous data windows") {
  std::array<std::string, 64> keys{};
  std::array<SignalMetadata, 64> rows{};
  for (std::size_t index = 0; index < rows.size(); ++index) {
    keys[index] = "signal." + std::to_string(index);
    rows[index] = boolean_signal(static_cast<std::uint16_t>(index + 1), keys[index]);
  }
  const SignalCatalogExport catalog{SignalCatalogView{rows.data(), rows.size()}};
  REQUIRE(catalog.available());
  REQUIRE(catalog.document_length() > kSignalCatalogPageDataBytes * 2);
  const std::uint32_t token = 0x78563412U;

  const auto first = catalog.encode_page(token, 0);
  REQUIRE(first.has_value());
  CHECK(first->size() == kMaxSignalCatalogPageBytes);
  CHECK((*first)[0] == kSignalCatalogFormat);
  CHECK((*first)[1] == 0x12);
  CHECK((*first)[2] == 0x34);
  CHECK((*first)[3] == 0x56);
  CHECK((*first)[4] == 0x78);
  CHECK(u16_at(first->view(), 5) == catalog.document_length());
  CHECK(u16_at(first->view(), 7) == 0);
  CHECK(u16_at(first->view(), 9) == kSignalCatalogPageDataBytes);
  const auto first_repeat = catalog.encode_page(token, 0);
  REQUIRE(first_repeat.has_value());
  CHECK(page_bytes(*first_repeat) == page_bytes(*first));

  const auto second = catalog.encode_page(token, kSignalCatalogPageDataBytes);
  REQUIRE(second.has_value());
  CHECK((*second)[0] == kSignalCatalogFormat);
  CHECK(u16_at(second->view(), 7) == kSignalCatalogPageDataBytes);
  CHECK(u16_at(second->view(), 9) == kSignalCatalogPageDataBytes);
  const auto second_repeat = catalog.encode_page(token, kSignalCatalogPageDataBytes);
  REQUIRE(second_repeat.has_value());
  CHECK(page_bytes(*second_repeat) == page_bytes(*second));

  const auto final =
      catalog.encode_page(token, static_cast<std::uint16_t>(catalog.document_length() - 1));
  REQUIRE(final.has_value());
  CHECK(final->size() == kSignalCatalogPageHeaderBytes + 1);
  CHECK(u16_at(final->view(), 9) == 1);
  const auto final_repeat =
      catalog.encode_page(token, static_cast<std::uint16_t>(catalog.document_length() - 1));
  REQUIRE(final_repeat.has_value());
  CHECK(page_bytes(*final_repeat) == page_bytes(*final));

  const auto terminal = catalog.encode_page(token, catalog.document_length());
  REQUIRE(terminal.has_value());
  CHECK(terminal->size() == kSignalCatalogPageHeaderBytes);
  CHECK(u16_at(terminal->view(), 9) == 0);
  const auto terminal_repeat = catalog.encode_page(token, catalog.document_length());
  REQUIRE(terminal_repeat.has_value());
  CHECK(page_bytes(*terminal_repeat) == page_bytes(*terminal));
  CHECK_FALSE(catalog.encode_page(token, static_cast<std::uint16_t>(catalog.document_length() + 1))
                  .has_value());

  const auto document = download_document(catalog, token);
  REQUIRE(document.has_value());
  CHECK(document->size() == catalog.document_length());
  CHECK(decode_catalog(*document).has_value());
}

TEST_CASE("same-token reconnect can rediscover every page from the stable service export") {
  std::array<std::string, 64> keys{};
  std::array<SignalMetadata, 64> rows{};
  for (std::size_t index = 0; index < rows.size(); ++index) {
    keys[index] = "signal." + std::to_string(index);
    rows[index] = boolean_signal(static_cast<std::uint16_t>(index + 1), keys[index]);
  }
  const SignalCatalogExport service_export{SignalCatalogView{rows.data(), rows.size()}};
  REQUIRE(service_export.available());
  REQUIRE(service_export.document_length() > kSignalCatalogPageDataBytes * 2);
  constexpr std::uint32_t token = 0x12345678U;

  // A reconnect starts a fresh complete download even when its token repeats.
  const auto first_connection = download_document(service_export, token);
  const auto second_connection = download_document(service_export, token);
  REQUIRE(first_connection.has_value());
  REQUIRE(second_connection.has_value());
  CHECK(*second_connection == *first_connection);
  const auto rediscovered = decode_catalog(*second_connection);
  REQUIRE(rediscovered.has_value());
  REQUIRE(rediscovered->size() == rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    CHECK((*rediscovered)[index].key == keys[index]);
    CHECK((*rediscovered)[index].handle == index + 1);
    CHECK(service_export.provider_id_for_handle((*rediscovered)[index].handle) == rows[index].id);
  }
  for (std::uint16_t offset = 0; offset < service_export.document_length();
       offset = static_cast<std::uint16_t>(offset + kSignalCatalogPageDataBytes)) {
    const auto first_page = service_export.encode_page(token, offset);
    const auto second_page = service_export.encode_page(token, offset);
    REQUIRE(first_page.has_value());
    REQUIRE(second_page.has_value());
    CHECK(page_bytes(*second_page) == page_bytes(*first_page));
  }
}

TEST_CASE("reordered signals and choices preserve discovered semantics after runtime renumbering") {
  const SignalEnumChoice original_choices[]{{100, "park"}, {200, "drive"}};
  const SignalEnumChoice reordered_choices[]{{42, "drive"}, {7, "park"}};
  const SignalMetadata original_rows[]{
      boolean_signal(10, "vehicle.door_open"),
      {SignalId{20}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, original_choices, 2},
  };
  const SignalMetadata reordered_rows[]{
      {SignalId{90}, "vehicle.gear", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Confirmed, SignalCapability::Read, reordered_choices, 2},
      boolean_signal(80, "vehicle.door_open"),
  };
  const SignalCatalogExport original{SignalCatalogView{original_rows}};
  const SignalCatalogExport reordered{SignalCatalogView{reordered_rows}};
  const auto original_document = download_document(original, 1);
  const auto reordered_document = download_document(reordered, 1);
  REQUIRE(original_document.has_value());
  REQUIRE(reordered_document.has_value());
  CHECK((*original_document)[0] == (*reordered_document)[0]);
  const auto original_map = decode_catalog(*original_document);
  const auto reordered_map = decode_catalog(*reordered_document);
  REQUIRE(original_map.has_value());
  REQUIRE(reordered_map.has_value());
  REQUIRE(original_map->size() == 2);
  REQUIRE(reordered_map->size() == 2);
  CHECK((*reordered_map)[0].key == (*original_map)[1].key);
  CHECK((*reordered_map)[0].type == (*original_map)[1].type);
  CHECK((*reordered_map)[0].unit == (*original_map)[1].unit);
  CHECK((*reordered_map)[0].evidence == (*original_map)[1].evidence);
  CHECK((*reordered_map)[1].key == (*original_map)[0].key);
  CHECK(reordered.provider_id_for_handle((*reordered_map)[0].handle) == SignalId{90});
  CHECK(reordered.provider_id_for_handle((*reordered_map)[1].handle) == SignalId{80});
  REQUIRE((*original_map)[1].choices.size() == 2);
  REQUIRE((*reordered_map)[0].choices.size() == 2);
  CHECK((*reordered_map)[0].choices[0].key == (*original_map)[1].choices[1].key);
  CHECK((*reordered_map)[0].choices[1].key == (*original_map)[1].choices[0].key);
  CHECK(reordered.wire_choice_handle((*reordered_map)[0].handle, 42) ==
        (*reordered_map)[0].choices[0].handle);
  CHECK(reordered.wire_choice_handle((*reordered_map)[0].handle, 7) ==
        (*reordered_map)[0].choices[1].handle);
}

TEST_CASE("catalog accepts 64 exported signals and rejects a 65th") {
  std::array<std::string, 65> keys{};
  std::array<SignalMetadata, 65> rows{};
  for (std::size_t index = 0; index < rows.size(); ++index) {
    keys[index] = "signal." + std::to_string(index);
    rows[index] = boolean_signal(static_cast<std::uint16_t>(index + 1), keys[index]);
  }
  const SignalCatalogExport maximum{SignalCatalogView{rows.data(), 64}};
  const SignalCatalogExport overflow{SignalCatalogView{rows.data(), rows.size()}};
  CHECK(maximum.available());
  CHECK(maximum.signal_count() == 64);
  CHECK_FALSE(overflow.available());
  CHECK(overflow.signal_count() == 0);
  CHECK(overflow.document_length() == 0);
  CHECK_FALSE(overflow.encode_page(1, 0).has_value());
}

TEST_CASE("catalog accepts the declared key and choice bounds and rejects over-limit metadata") {
  const std::string key63 = fixed_key("signal", 63);
  const SignalMetadata maximum_key_rows[]{
      boolean_signal(1, key63),
  };
  const SignalCatalogExport maximum_key{SignalCatalogView{maximum_key_rows}};
  CHECK(maximum_key.available());

  const std::string utf8_key{"signal.\xE2\x82\xAC", 10};
  const SignalMetadata utf8_rows[]{boolean_signal(1, utf8_key)};
  CHECK(SignalCatalogExport{SignalCatalogView{utf8_rows}}.available());

  const std::string key64 = fixed_key("signal", 64);
  const SignalMetadata oversized_key_rows[]{
      boolean_signal(1, key64),
  };
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{oversized_key_rows}}.available());

  std::array<std::string, 32> choice_keys{};
  std::array<SignalEnumChoice, 32> choices{};
  for (std::size_t index = 0; index < choices.size(); ++index) {
    choice_keys[index] = "choice." + std::to_string(index);
    choices[index] =
        SignalEnumChoice{static_cast<std::uint16_t>(0x1000 + index), choice_keys[index]};
  }
  const SignalMetadata max_choices_rows[]{
      {SignalId{1}, "vehicle.mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
       SignalCapability::Read, choices.data(), choices.size()},
  };
  const SignalCatalogExport max_choices{SignalCatalogView{max_choices_rows}};
  CHECK(max_choices.available());
  CHECK(max_choices.signal_count() == 1);

  std::array<SignalEnumChoice, 33> too_many_choices{};
  std::array<std::string, 33> too_many_keys{};
  for (std::size_t index = 0; index < too_many_choices.size(); ++index) {
    too_many_keys[index] = "choice." + std::to_string(index);
    too_many_choices[index] =
        SignalEnumChoice{static_cast<std::uint16_t>(0x2000 + index), too_many_keys[index]};
  }
  const SignalMetadata too_many_rows[]{
      {SignalId{1}, "vehicle.mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
       SignalCapability::Read, too_many_choices.data(), too_many_choices.size()},
  };
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{too_many_rows}}.available());

  std::array<std::string, 32> max_choice_text{};
  std::array<SignalEnumChoice, 32> max_choice_rows{};
  for (std::size_t index = 0; index < max_choice_rows.size(); ++index) {
    max_choice_text[index] = fixed_key("c" + std::to_string(index), 63);
    max_choice_rows[index] =
        SignalEnumChoice{static_cast<std::uint16_t>(index + 1), max_choice_text[index]};
  }
  const SignalMetadata max_choice_key_rows[]{
      {SignalId{1}, "vehicle.mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
       SignalCapability::Read, max_choice_rows.data(), max_choice_rows.size()},
  };
  CHECK(SignalCatalogExport{SignalCatalogView{max_choice_key_rows}}.available());

  max_choice_text[0] = fixed_key("c0", 64);
  max_choice_rows[0].key = max_choice_text[0];
  const SignalMetadata long_choice_key_rows[]{
      {SignalId{1}, "vehicle.mode", SignalType::Enum, SignalUnit::None, ValidationStatus::Reference,
       SignalCapability::Read, max_choice_rows.data(), max_choice_rows.size()},
  };
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{long_choice_key_rows}}.available());
}

TEST_CASE("catalog accepts exactly 16 KiB and rejects the first byte beyond the document bound") {
  constexpr std::size_t kChoiceCount = 32;
  std::array<std::array<std::string, kChoiceCount>, 8> choice_text{};
  std::array<std::array<SignalEnumChoice, kChoiceCount>, 8> choices{};
  std::array<std::string, 8> signal_keys{};
  std::array<SignalMetadata, 8> rows{};

  for (std::size_t signal = 0; signal < 8; ++signal) {
    signal_keys[signal] = fixed_key("s" + std::to_string(signal), 63);
    for (std::size_t choice = 0; choice < kChoiceCount; ++choice) {
      std::size_t length = 63;
      if (signal == 7) {
        length = choice < 11 ? 63 : (choice == 11 ? 4 : 1);
      }
      std::string prefix = "c" + std::to_string(choice) + "-" + std::to_string(signal) + "-";
      if (signal == 7 && choice == 11) {
        prefix = "four";
      } else if (signal == 7 && choice > 11) {
        prefix = std::string(1, static_cast<char>('!' + choice - 12));
      }
      choice_text[signal][choice] = fixed_key(prefix, length);
      choices[signal][choice] =
          SignalEnumChoice{static_cast<std::uint16_t>(100 + choice), choice_text[signal][choice]};
    }
    rows[signal] = {SignalId{static_cast<std::uint16_t>(signal + 1)},
                    signal_keys[signal],
                    SignalType::Enum,
                    SignalUnit::None,
                    ValidationStatus::Reference,
                    SignalCapability::Read,
                    choices[signal].data(),
                    kChoiceCount};
  }

  const SignalCatalogExport exact{SignalCatalogView{rows.data(), rows.size()}};
  REQUIRE(exact.available());
  CHECK(exact.document_length() == kMaxSignalCatalogDocumentBytes);
  const auto exact_document = download_document(exact, 1);
  REQUIRE(exact_document.has_value());
  CHECK(exact_document->size() == kMaxSignalCatalogDocumentBytes);
  CHECK(decode_catalog(*exact_document).has_value());

  choice_text[7][31].push_back('y');
  choices[7][31].key = choice_text[7][31];
  const SignalCatalogExport oversized{SignalCatalogView{rows.data(), rows.size()}};
  CHECK_FALSE(oversized.available());
  CHECK(oversized.signal_count() == 0);
  CHECK(oversized.document_length() == 0);
}

TEST_CASE("catalog is all-or-unavailable for malformed metadata and duplicate identities") {
  const SignalMetadata zero_id_rows[]{boolean_signal(0, "valid.key")};
  const SignalMetadata empty_key_rows[]{boolean_signal(1, "")};
  const std::string invalid_utf8{"\xC0\xAF", 2};
  const SignalMetadata invalid_utf8_rows[]{boolean_signal(1, invalid_utf8)};
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{zero_id_rows}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{empty_key_rows}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{invalid_utf8_rows}}.available());

  const SignalMetadata duplicate_ids[]{boolean_signal(1, "one"), boolean_signal(1, "two")};
  const SignalMetadata duplicate_keys[]{boolean_signal(1, "same"), boolean_signal(2, "same")};
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{duplicate_ids}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{duplicate_keys}}.available());

  const SignalMetadata malformed_unit[]{
      {SignalId{1}, "boolean.with.unit", SignalType::Boolean, SignalUnit::KilometresPerHour,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
  };
  const SignalMetadata enum_without_choices[]{
      {SignalId{1}, "enum.without.choices", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
  };
  const SignalEnumChoice unrelated_choice[]{{1, "unused"}};
  const SignalMetadata non_enum_with_choices[]{
      {SignalId{1}, "number.with.choices", SignalType::Number, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, unrelated_choice, 1},
  };
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{malformed_unit}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{enum_without_choices}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{non_enum_with_choices}}.available());

  const SignalEnumChoice duplicate_values[]{{1, "one"}, {1, "two"}};
  const SignalEnumChoice duplicate_choice_keys[]{{1, "same"}, {2, "same"}};
  const SignalMetadata duplicate_value_rows[]{
      {SignalId{1}, "enum.duplicate.values", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, duplicate_values, 2},
  };
  const SignalMetadata duplicate_choice_key_rows[]{
      {SignalId{1}, "enum.duplicate.keys", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, duplicate_choice_keys, 2},
  };
  const std::string invalid_choice_utf8{"\xED\xA0\x80", 3};
  const SignalEnumChoice invalid_choice_rows[]{{1, invalid_choice_utf8}};
  const SignalMetadata invalid_choice_catalog[]{
      {SignalId{1}, "enum.invalid.utf8", SignalType::Enum, SignalUnit::None,
       ValidationStatus::Reference, SignalCapability::Read, invalid_choice_rows, 1},
  };
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{duplicate_value_rows}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{duplicate_choice_key_rows}}.available());
  CHECK_FALSE(SignalCatalogExport{SignalCatalogView{invalid_choice_catalog}}.available());
}
