#include "companion_protocol/signal_catalog_export.hpp"

#include <limits>
#include <string_view>

namespace companion_protocol {

namespace {

using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::ValidationStatus;

constexpr std::size_t kCatalogDocumentHeaderBytes = 3;
constexpr std::size_t kDescriptorLengthBytes = 2;
constexpr std::size_t kSignalHandleBytes = 2;
constexpr std::size_t kTlvHeaderBytes = 3;

std::string_view unit_token(SignalUnit unit) noexcept {
  switch (unit) {
  case SignalUnit::None:
    return "none";
  case SignalUnit::KilometresPerHour:
    return "km/h";
  case SignalUnit::RevolutionsPerMinute:
    return "rpm";
  case SignalUnit::MetresPerSecondSquared:
    return "m/s^2";
  }
  return "unknown";
}

std::string_view evidence_token(ValidationStatus status) noexcept {
  switch (status) {
  case ValidationStatus::Reference:
    return "reference";
  case ValidationStatus::Observed:
    return "observed";
  case ValidationStatus::Confirmed:
    return "confirmed";
  }
  return "unknown";
}

bool wire_type_code(SignalType type, std::uint8_t &code) noexcept {
  switch (type) {
  case SignalType::Boolean:
    code = 1;
    return true;
  case SignalType::Number:
    code = 2;
    return true;
  case SignalType::Enum:
    code = 3;
    return true;
  }
  return false;
}

bool valid_utf8(std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto lead = static_cast<std::uint8_t>(text[offset]);
    if (lead <= 0x7FU) {
      ++offset;
      continue;
    }

    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    std::size_t continuation_count = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      code_point = lead & 0x1FU;
      minimum = 0x80U;
      continuation_count = 1;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      code_point = lead & 0x0FU;
      minimum = 0x800U;
      continuation_count = 2;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      code_point = lead & 0x07U;
      minimum = 0x10000U;
      continuation_count = 3;
    } else {
      return false;
    }

    if (offset + continuation_count >= text.size()) {
      return false;
    }
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool valid_wire_string(std::string_view text, std::size_t maximum_bytes) noexcept {
  return !text.empty() && text.size() <= maximum_bytes && valid_utf8(text);
}

std::size_t tlv_bytes(std::size_t value_length) noexcept { return kTlvHeaderBytes + value_length; }

std::size_t choices_value_bytes(const SignalMetadata &metadata) noexcept {
  std::size_t size = 2;
  for (std::size_t index = 0; index < metadata.choice_count; ++index) {
    size += 2 + 1 + metadata.choices[index].key.size();
  }
  return size;
}

std::size_t descriptor_body_bytes(const SignalMetadata &metadata) noexcept {
  const std::string_view unit = unit_token(metadata.unit);
  const std::string_view evidence = evidence_token(metadata.validation);
  std::size_t size = kSignalHandleBytes + tlv_bytes(metadata.key.size()) + tlv_bytes(1) +
                     tlv_bytes(unit.size()) + tlv_bytes(evidence.size());
  if (metadata.type == SignalType::Enum) {
    size += tlv_bytes(choices_value_bytes(metadata));
  }
  return size;
}

class PageSink final {
public:
  PageSink(EncodedSignalCatalogPage &page, std::size_t begin, std::size_t length) noexcept
      : page_(page), begin_(begin), end_(begin + length) {}

  void push(std::uint8_t byte) noexcept {
    if (cursor_ >= begin_ && cursor_ < end_ && !page_.push(byte)) {
      ok_ = false;
    }
    ++cursor_;
  }

  void push_u16(std::uint16_t value) noexcept {
    push(static_cast<std::uint8_t>(value & 0xFFU));
    push(static_cast<std::uint8_t>(value >> 8U));
  }

  void append(std::string_view text) noexcept {
    for (const char byte : text) {
      push(static_cast<std::uint8_t>(byte));
    }
  }

  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
  EncodedSignalCatalogPage &page_;
  std::size_t begin_{0};
  std::size_t end_{0};
  std::size_t cursor_{0};
  bool ok_{true};
};

void append_tlv(PageSink &sink, std::uint8_t tag, std::string_view value) noexcept {
  sink.push(tag);
  sink.push_u16(static_cast<std::uint16_t>(value.size()));
  sink.append(value);
}

} // namespace

SignalCatalogExport::SignalCatalogExport(vehicle_signals::SignalCatalogView catalog) noexcept {
  if (!catalog.well_formed()) {
    return;
  }

  std::size_t document_length = kCatalogDocumentHeaderBytes;
  for (std::size_t catalog_index = 0; catalog_index < catalog.size(); ++catalog_index) {
    const SignalMetadata *metadata = catalog.at(catalog_index);
    if (metadata == nullptr ||
        !metadata->capabilities.has(vehicle_signals::SignalCapability::Read)) {
      continue;
    }

    std::uint8_t type_code = 0;
    if (!wire_type_code(metadata->type, type_code)) {
      continue;
    }
    (void)type_code;
    if (signal_count_ >= kMaxSignalCatalogSignals ||
        !valid_wire_string(metadata->key, kMaxSignalCatalogKeyBytes)) {
      return;
    }

    const std::string_view unit = unit_token(metadata->unit);
    const std::string_view evidence = evidence_token(metadata->validation);
    if (!valid_wire_string(unit, kMaxSignalCatalogTokenBytes) ||
        !valid_wire_string(evidence, kMaxSignalCatalogTokenBytes)) {
      return;
    }
    if (metadata->type == SignalType::Enum) {
      if (metadata->choices == nullptr || metadata->choice_count == 0 ||
          metadata->choice_count > kMaxSignalCatalogChoices) {
        return;
      }
      for (std::size_t choice_index = 0; choice_index < metadata->choice_count; ++choice_index) {
        if (!valid_wire_string(metadata->choices[choice_index].key, kMaxSignalCatalogKeyBytes)) {
          return;
        }
      }
    }

    const std::size_t body_length = descriptor_body_bytes(*metadata);
    if (body_length > std::numeric_limits<std::uint16_t>::max()) {
      return;
    }
    const std::size_t descriptor_length = kDescriptorLengthBytes + body_length;
    if (descriptor_length > kMaxSignalCatalogDocumentBytes ||
        document_length > kMaxSignalCatalogDocumentBytes - descriptor_length) {
      return;
    }
    document_length += descriptor_length;
    entries_[signal_count_] = metadata;
    ++signal_count_;
  }

  document_length_ = static_cast<std::uint16_t>(document_length);
  available_ = true;
}

std::optional<EncodedSignalCatalogPage>
SignalCatalogExport::encode_page(std::uint32_t token, std::uint16_t offset) const noexcept {
  if (!available_ || token == 0 || offset > document_length_) {
    return std::nullopt;
  }

  const std::size_t remaining = document_length_ - offset;
  const std::size_t data_length =
      remaining < kSignalCatalogPageDataBytes ? remaining : kSignalCatalogPageDataBytes;
  EncodedSignalCatalogPage page{};
  if (!page.push(kSignalCatalogFormat) || !page.push_u32(token) ||
      !page.push_u16(document_length_) || !page.push_u16(offset) ||
      !page.push_u16(static_cast<std::uint16_t>(data_length))) {
    return std::nullopt;
  }

  PageSink sink{page, offset, data_length};
  sink.push(kSignalCatalogFormat);
  sink.push_u16(signal_count_);
  for (std::uint16_t signal_index = 0; signal_index < signal_count_; ++signal_index) {
    const SignalMetadata &metadata = *entries_[signal_index];
    const std::uint16_t handle = static_cast<std::uint16_t>(signal_index + 1U);
    const std::uint16_t body_length = static_cast<std::uint16_t>(descriptor_body_bytes(metadata));
    std::uint8_t type_code = 0;
    if (!wire_type_code(metadata.type, type_code)) {
      return std::nullopt;
    }

    sink.push_u16(body_length);
    sink.push_u16(handle);
    append_tlv(sink, 1, metadata.key);
    sink.push(2);
    sink.push_u16(1);
    sink.push(type_code);
    append_tlv(sink, 3, unit_token(metadata.unit));
    append_tlv(sink, 4, evidence_token(metadata.validation));
    if (metadata.type == SignalType::Enum) {
      sink.push(5);
      sink.push_u16(static_cast<std::uint16_t>(choices_value_bytes(metadata)));
      sink.push_u16(static_cast<std::uint16_t>(metadata.choice_count));
      for (std::size_t choice_index = 0; choice_index < metadata.choice_count; ++choice_index) {
        const std::uint16_t choice_handle = static_cast<std::uint16_t>(choice_index + 1U);
        const std::string_view key = metadata.choices[choice_index].key;
        sink.push_u16(choice_handle);
        sink.push(static_cast<std::uint8_t>(key.size()));
        sink.append(key);
      }
    }
  }

  if (!sink.ok() || sink.cursor() != document_length_ ||
      page.size() != kSignalCatalogPageHeaderBytes + data_length) {
    return std::nullopt;
  }
  return page;
}

vehicle_signals::SignalId
SignalCatalogExport::provider_id_for_handle(std::uint16_t wire_handle) const noexcept {
  const SignalMetadata *metadata = metadata_for_handle(wire_handle);
  return metadata == nullptr ? vehicle_signals::SignalId{} : metadata->id;
}

std::optional<std::uint16_t>
SignalCatalogExport::wire_choice_handle(std::uint16_t wire_handle,
                                        std::uint16_t provider_choice_value) const noexcept {
  const SignalMetadata *metadata = metadata_for_handle(wire_handle);
  if (metadata == nullptr || metadata->type != SignalType::Enum) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < metadata->choice_count; ++index) {
    if (metadata->choices[index].value == provider_choice_value) {
      return static_cast<std::uint16_t>(index + 1U);
    }
  }
  return std::nullopt;
}

const vehicle_signals::SignalMetadata *
SignalCatalogExport::metadata_for_handle(std::uint16_t wire_handle) const noexcept {
  if (!available_ || wire_handle == 0 || wire_handle > signal_count_) {
    return nullptr;
  }
  return entries_[wire_handle - 1U];
}

} // namespace companion_protocol
